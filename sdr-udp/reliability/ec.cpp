#include "reliability/ec.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include "reliability/sr.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#if defined(HAS_ISAL) && __has_include(<isa-l/erasure_code.h>)
#include <isa-l/erasure_code.h>
#else
#undef HAS_ISAL
#endif

namespace sdr::reliability {

int ECSender::encode_and_send(SDRConnection* conn, const void* buffer, size_t length) {
    conn_ = conn;
    sends_.clear();
    // Use existing connection params (set via CTS in sdr_api paths); fall back to defaults
    ConnectionParams params = conn->connection_ctx->get_params();
    uint32_t mtu = params.mtu_bytes ? params.mtu_bytes : SDRPacket::MAX_PAYLOAD_SIZE;
    if (mtu > SDRPacket::MAX_PAYLOAD_SIZE) {
        mtu = SDRPacket::MAX_PAYLOAD_SIZE;
    }
    uint16_t ppc = params.packets_per_chunk ? params.packets_per_chunk : 32;
    uint32_t chunk_bytes = mtu * ppc;
    if (chunk_bytes == 0) chunk_bytes = SDRPacket::MAX_PAYLOAD_SIZE;

    const uint64_t data_bytes = cfg_.data_bytes ? cfg_.data_bytes : length;
    const uint16_t k = cfg_.k_data ? cfg_.k_data : 4;
    const uint16_t m = cfg_.m_parity ? cfg_.m_parity : 2;

    uint32_t data_chunks = static_cast<uint32_t>((data_bytes + chunk_bytes - 1) / chunk_bytes);
    uint32_t stripes = (data_chunks + k - 1) / k;
    uint32_t parity_chunks = stripes * m;
    uint64_t total_chunks = data_chunks + parity_chunks;
    uint64_t total_bytes = total_chunks * chunk_bytes;

    send_storage_.assign(static_cast<size_t>(total_bytes), 0);
    std::memcpy(send_storage_.data(), buffer, std::min<uint64_t>(data_bytes, total_bytes));

#ifdef HAS_ISAL
    std::vector<uint8_t*> data_ptrs(k);
    std::vector<uint8_t*> parity_ptrs(m);
    std::vector<uint8_t> encode_matrix(k * (k + m));
    std::vector<uint8_t> gftbl(m * k * 32);
    gf_gen_rs_matrix(encode_matrix.data(), k + m, k);
    ec_init_tables(k, m, encode_matrix.data(), gftbl.data());

    for (uint32_t s = 0; s < stripes; ++s) {
        uint32_t stripe_data = std::min<uint32_t>(k, data_chunks - s * k);
        for (uint32_t i = 0; i < stripe_data; ++i) {
            data_ptrs[i] = send_storage_.data() + (s * k + i) * chunk_bytes;
        }
        // Zero-pad remaining data pointers if last stripe is partial
        for (uint32_t i = stripe_data; i < k; ++i) {
            data_ptrs[i] = send_storage_.data(); // any valid pointer; data is already zeroed
        }
        for (uint32_t p = 0; p < m; ++p) {
            parity_ptrs[p] = send_storage_.data() + (data_chunks + s * m + p) * chunk_bytes;
        }
        ec_encode_data(chunk_bytes, k, m, gftbl.data(), data_ptrs.data(), parity_ptrs.data());
    }
#else
    std::cerr << "[EC] ISA-L not available; cannot encode parity\n";
    return -1;
#endif

    SDRSendHandle* raw_handle = nullptr;
    int rc = sdr_send_post(conn, send_storage_.data(), send_storage_.size(), &raw_handle);
    if (rc != 0) {
        std::cerr << "[EC] Failed to send data+parity buffer\n";
        return rc;
    }
    sends_.emplace_back(raw_handle, [](SDRSendHandle* h){ delete h; });
    return 0;
}

int ECSender::poll() {
    if (sends_.empty() || !conn_ || !conn_->tcp_client) {
        return -1;
    }
    auto* handle = sends_.front().get();
    const ConnectionParams& params = conn_->connection_ctx->get_params();
    uint32_t mtu = params.mtu_bytes ? params.mtu_bytes : SDRPacket::MAX_PAYLOAD_SIZE;
    if (mtu > SDRPacket::MAX_PAYLOAD_SIZE) mtu = SDRPacket::MAX_PAYLOAD_SIZE;
    uint16_t ppc = params.packets_per_chunk ? params.packets_per_chunk : 32;
    uint32_t chunk_bytes = mtu * ppc;
    uint32_t data_chunks = static_cast<uint32_t>((cfg_.data_bytes + chunk_bytes - 1) / chunk_bytes);

    auto retransmit_chunk = [&](uint32_t chunk_id) {
        int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_socket < 0) return;
        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(params.udp_server_port);
        inet_pton(AF_INET, params.udp_server_ip, &server_addr.sin_addr);

        const uint8_t* data = static_cast<const uint8_t*>(handle->user_buffer);
        for (uint16_t pkt = 0; pkt < ppc; ++pkt) {
            uint32_t packet_offset = chunk_id * ppc + pkt;
            size_t data_offset = static_cast<size_t>(packet_offset) * mtu;
            if (data_offset >= handle->buffer_size) break;
            size_t remaining = handle->buffer_size - data_offset;
            size_t pkt_len = std::min(static_cast<size_t>(mtu), remaining);
            SDRPacket* packet = SDRPacket::create_data_packet(
                params.transfer_id, handle->msg_id, packet_offset, ppc,
                data + data_offset, pkt_len);
            if (!packet) break;
            packet->header.chunk_seq = packet->header.get_chunk_id();
            size_t total_sz = sizeof(SDRPacketHeader) + pkt_len;
            packet->header.to_network_order();
            sendto(udp_socket, packet, total_sz, 0,
                   (struct sockaddr*)&server_addr, sizeof(server_addr));
            SDRPacket::destroy(packet);
        }
        close(udp_socket);
    };

    auto apply_bitmap = [&](const ControlMessage& msg) {
        uint32_t words = msg.chunk_bitmap_words;
        uint32_t max_chunks = data_chunks + (msg.chunk_bitmap_words * 64 > data_chunks ? msg.chunk_bitmap_words * 64 - data_chunks : 0);
        for (uint32_t w = 0; w < words && w < 16; ++w) {
            uint64_t word = msg.chunk_bitmap[w];
            for (uint32_t bit = 0; bit < 64; ++bit) {
                uint32_t chunk_id = w * 64 + bit;
                if (chunk_id >= data_chunks) break;
                if (word & (1ULL << bit)) {
                    // mark acked by skipping retransmit
                }
            }
        }
    };

    auto retransmit_missing_bitmap = [&](const ControlMessage& msg, uint32_t limit) {
        uint32_t sent = 0;
        uint32_t words = msg.chunk_bitmap_words;
        for (uint32_t w = 0; w < words && sent < limit && w < 16; ++w) {
            uint64_t word = msg.chunk_bitmap[w];
            for (uint32_t bit = 0; bit < 64 && sent < limit; ++bit) {
                uint32_t chunk_id = w * 64 + bit;
                if (chunk_id >= data_chunks) break;
                if ((word & (1ULL << bit)) == 0) {
                    retransmit_chunk(chunk_id);
                    sent++;
                }
            }
        }
    };

    while (true) {
        ControlMessage msg;
        if (!conn_->tcp_client->receive_message(msg)) {
            // Timeout, check completion
            int rc = sdr_send_poll(handle);
            if (rc == 0) return 0;
            continue;
        }
        if (msg.msg_type == ControlMsgType::EC_ACK) {
            return 0;
        } else if (msg.msg_type == ControlMsgType::EC_NACK) {
            for (uint16_t i = 0; i < msg.num_gaps; ++i) {
                uint32_t start = msg.gap_start[i];
                uint32_t len = msg.gap_len[i];
                for (uint32_t c = start; c < start + len && c < data_chunks; ++c) {
                    retransmit_chunk(c);
                }
            }
            retransmit_missing_bitmap(msg, 8);
        } else if (msg.msg_type == ControlMsgType::EC_FALLBACK_SR) {
            SRConfig sr_cfg{};
            sr_cfg.rto_ms = cfg_.fallback_timeout_ms ? cfg_.fallback_timeout_ms : 500;
            sr_cfg.nack_delay_ms = 200;
            sr_cfg.max_inflight_chunks = 0;
            SRSender sr_sender(sr_cfg);
            size_t data_len = cfg_.data_bytes ? cfg_.data_bytes : send_storage_.size();
            conn_->connection_ctx->set_auto_send_data(false);
            if (sr_sender.start_send(conn_, send_storage_.data(), data_len) != 0) {
                conn_->connection_ctx->set_auto_send_data(true);
                return -1;
            }
            int rc = 0;
            while ((rc = sr_sender.poll()) > 0) {
                // keep polling
            }
            conn_->connection_ctx->set_auto_send_data(true);
            return rc == 0 ? 0 : -1;
        } else if (msg.msg_type == ControlMsgType::COMPLETE_ACK) {
            return 0;
        }
    }
}

int ECReceiver::post_receive(SDRConnection* conn, void* buffer, size_t length) {
    conn_ = conn;
    uint32_t mtu = conn->connection_ctx->get_params().mtu_bytes ? conn->connection_ctx->get_params().mtu_bytes : SDRPacket::MAX_PAYLOAD_SIZE;
    if (mtu > SDRPacket::MAX_PAYLOAD_SIZE) {
        mtu = SDRPacket::MAX_PAYLOAD_SIZE;
    }
    uint16_t ppc = conn->connection_ctx->get_params().packets_per_chunk ? conn->connection_ctx->get_params().packets_per_chunk : 32;
    chunk_bytes_ = mtu * ppc;
    if (chunk_bytes_ == 0) chunk_bytes_ = SDRPacket::MAX_PAYLOAD_SIZE;

    data_bytes_ = cfg_.data_bytes ? cfg_.data_bytes : length;
    k_ = cfg_.k_data ? cfg_.k_data : 4;
    m_ = cfg_.m_parity ? cfg_.m_parity : 2;
    data_chunks_ = static_cast<uint32_t>((data_bytes_ + chunk_bytes_ - 1) / chunk_bytes_);
    stripes_ = (data_chunks_ + k_ - 1) / k_;
    parity_chunks_ = stripes_ * m_;
    size_t required_length = static_cast<size_t>(data_chunks_ + parity_chunks_) * chunk_bytes_;
    if (length < required_length) {
        std::cerr << "[EC] Receiver buffer too small for data+parity\n";
        return -1;
    }
    decode_attempts_ = 0;
    fallback_active_ = false;

    SDRRecvHandle* raw_handle = nullptr;
    int rc = sdr_recv_post(conn, buffer, length, &raw_handle);
    if (rc != 0) {
        std::cerr << "[EC] Failed to post receive\n";
        return rc;
    }
    recv_handle_.reset(raw_handle);
    // Set expected chunks for progress
    if (recv_handle_->msg_ctx) {
        recv_handle_->msg_ctx->total_chunks = data_chunks_ + parity_chunks_;
    }
    return 0;
}

bool ECReceiver::try_decode() {
    if (!recv_handle_) return false;
    const uint8_t* bitmap = nullptr;
    size_t len = 0;
    if (sdr_recv_bitmap_get(recv_handle_.get(), &bitmap, &len) != 0) {
        return false;
    }
    auto* ctx = recv_handle_->msg_ctx.get();
    if (!ctx || !ctx->frontend_bitmap) return false;

    // Count missing data chunks
    std::vector<uint32_t> missing_data;
    for (uint32_t c = 0; c < data_chunks_; ++c) {
        if (!ctx->frontend_bitmap->is_chunk_complete(c)) {
            missing_data.push_back(c);
        }
    }
    auto send_sr_control = [&](bool send_complete) {
        if (!conn_ || !conn_->tcp_server) return;
        ControlMessage msg{};
        msg.magic = ControlMessage::MAGIC_VALUE;
        msg.connection_id = conn_->connection_ctx->get_connection_id();
        msg.params.total_chunks = static_cast<uint16_t>(data_chunks_);
        // cumulative
        uint32_t cumulative = 0;
        while (cumulative < data_chunks_ && ctx->frontend_bitmap->is_chunk_complete(cumulative)) {
            cumulative++;
        }
        if (cumulative > 0) cumulative -= 1;
        msg.params.max_inflight = cumulative;
        // bitmap
        uint32_t word_count = (data_chunks_ + 63) / 64;
        if (word_count > 16) word_count = 16;
        msg.chunk_bitmap_words = static_cast<uint16_t>(word_count);
        for (uint32_t c = 0; c < data_chunks_ && c < 1024; ++c) {
            if (ctx->frontend_bitmap->is_chunk_complete(c)) {
                uint32_t w = c / 64;
                uint32_t b = c % 64;
                msg.chunk_bitmap[w] |= (1ULL << b);
            }
        }
        if (send_complete) {
            msg.msg_type = ControlMsgType::COMPLETE_ACK;
            conn_->tcp_server->send_message(msg);
            return;
        }
        // gaps
        uint16_t gaps_found = 0;
        for (uint32_t c = cumulative + 1; c < data_chunks_ && gaps_found < 4; ++c) {
            if (!ctx->frontend_bitmap->is_chunk_complete(c)) {
                uint16_t start = static_cast<uint16_t>(c);
                uint16_t run = 0;
                while (c < data_chunks_ && !ctx->frontend_bitmap->is_chunk_complete(c) && run < UINT16_MAX) {
                    ++run; ++c;
                }
                msg.gap_start[gaps_found] = start;
                msg.gap_len[gaps_found] = run;
                gaps_found++;
            }
        }
        msg.num_gaps = gaps_found;
        if (gaps_found > 0) {
            msg.msg_type = ControlMsgType::SR_NACK;
            msg.params.rto_ms = msg.gap_start[0];
            msg.params.rtt_alpha_ms = msg.gap_len[0];
        } else {
            msg.msg_type = ControlMsgType::SR_ACK;
        }
        conn_->tcp_server->send_message(msg);
    };

    if (fallback_active_) {
        // Drive SR control path during fallback
        send_sr_control(missing_data.empty());
        if (missing_data.empty()) {
            stats_.decode_success++;
            fallback_active_ = false;
            return true;
        }
    }
    if (missing_data.empty()) {
        stats_.decode_success++;
        // Send EC_ACK
        if (conn_ && conn_->tcp_server) {
            ControlMessage msg{};
            msg.magic = ControlMessage::MAGIC_VALUE;
            msg.msg_type = ControlMsgType::EC_ACK;
            msg.connection_id = conn_->connection_ctx->get_connection_id();
            conn_->tcp_server->send_message(msg);
        }
        return true;
    }
    if (missing_data.size() > m_) {
        // Send EC_NACK or trigger fallback SR
        if (conn_ && conn_->tcp_server) {
            ControlMessage msg{};
            msg.magic = ControlMessage::MAGIC_VALUE;
            msg.connection_id = conn_->connection_ctx->get_connection_id();
            msg.num_gaps = 0;
            // collapse missing_data into gaps
            size_t idx = 0;
            while (idx < missing_data.size() && msg.num_gaps < 16) {
                uint32_t start = missing_data[idx];
                uint32_t lenrun = 1;
                idx++;
                while (idx < missing_data.size() && missing_data[idx] == start + lenrun) {
                    lenrun++;
                    idx++;
                }
                msg.gap_start[msg.num_gaps] = static_cast<uint16_t>(start);
                msg.gap_len[msg.num_gaps] = static_cast<uint16_t>(lenrun);
                msg.num_gaps++;
            }
            if (decode_attempts_ + 1 >= cfg_.max_retries) {
                msg.msg_type = ControlMsgType::EC_FALLBACK_SR;
                fallback_active_ = true;
                stats_.fallback_sr++;
            } else {
                msg.msg_type = ControlMsgType::EC_NACK;
            }
            conn_->tcp_server->send_message(msg);
            std::cout << "[EC][Receiver] " << (msg.msg_type == ControlMsgType::EC_FALLBACK_SR ? "EC_FALLBACK_SR" : "EC_NACK")
                      << " gaps=" << static_cast<int>(msg.num_gaps) << std::endl;
        }
        decode_attempts_++;
        return false; // allow retries or fallback
    }

#ifdef HAS_ISAL
    // Build lists of available chunks (data+parity)
    std::vector<uint32_t> avail_idxs;
    for (uint32_t c = 0; c < data_chunks_ + parity_chunks_; ++c) {
        if (ctx->frontend_bitmap->is_chunk_complete(c)) {
            avail_idxs.push_back(c);
        }
    }
    if (avail_idxs.size() < k_) {
        stats_.fallback_sr++;
        return false;
    }

    std::vector<uint8_t*> src_ptrs(k_);
    std::vector<uint8_t*> recover_ptrs(m_);
    std::vector<uint8_t> encode_matrix((k_ + m_) * k_);
    std::vector<uint8_t> decode_matrix(k_ * k_);
    std::vector<uint8_t> gftbl(m_ * k_ * 32);

    gf_gen_rs_matrix(encode_matrix.data(), k_ + m_, k_);

    // Build decode matrix from first k available chunks
    for (uint32_t i = 0; i < k_; ++i) {
        uint32_t idx = avail_idxs[i];
        for (uint32_t j = 0; j < k_; ++j) {
            decode_matrix[i * k_ + j] = encode_matrix[idx * k_ + j];
        }
        src_ptrs[i] = static_cast<uint8_t*>(ctx->buffer) + idx * chunk_bytes_;
    }
    if (gf_invert_matrix(decode_matrix.data(), gftbl.data(), k_) < 0) {
        stats_.fallback_sr++;
        return false;
    }

    // Recover missing data chunks
    for (size_t mi = 0; mi < missing_data.size(); ++mi) {
        uint32_t idx = missing_data[mi];
        recover_ptrs[mi] = static_cast<uint8_t*>(ctx->buffer) + idx * chunk_bytes_;
        ec_init_tables(k_, 1, decode_matrix.data() + mi * k_, gftbl.data());
        ec_encode_data(chunk_bytes_, k_, 1, gftbl.data(), src_ptrs.data(), &recover_ptrs[mi]);
    }
    stats_.decode_success++;
    if (conn_ && conn_->tcp_server) {
        ControlMessage msg{};
        msg.magic = ControlMessage::MAGIC_VALUE;
        msg.msg_type = ControlMsgType::EC_ACK;
        msg.connection_id = conn_->connection_ctx->get_connection_id();
        conn_->tcp_server->send_message(msg);
    }
    return true;
#else
    stats_.fallback_sr++;
    return false;
#endif
}

} // namespace sdr::reliability
