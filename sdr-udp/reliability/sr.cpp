#include "reliability/sr.h"
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

namespace sdr::reliability {

int SRSender::start_send(SDRConnection* conn, const void* buffer, size_t length) {
    conn_ = conn;
    const ConnectionParams& params = conn_->connection_ctx->get_params();
    SDRSendHandle* raw_handle = nullptr;
    int rc = sdr_send_post(conn, buffer, length, &raw_handle);
    if (rc != 0) {
        std::cerr << "[SR] Failed to start send\n";
        return rc;
    }
    send_handle_.reset(raw_handle);

    // Initialize chunk tracking
    mtu_bytes_ = params.mtu_bytes == 0 ? SDRPacket::MAX_PAYLOAD_SIZE : params.mtu_bytes;
    packets_per_chunk_ = params.packets_per_chunk == 0 ? 1 : params.packets_per_chunk;
    uint64_t chunk_bytes = static_cast<uint64_t>(mtu_bytes_) * packets_per_chunk_;
    total_chunks_ = static_cast<uint32_t>((length + chunk_bytes - 1) / chunk_bytes);
    chunk_acked_.assign(total_chunks_, false);
    last_tx_.assign(total_chunks_, std::chrono::steady_clock::now());
    last_control_tx_ = std::chrono::steady_clock::now();
    return 0;
}

int SRSender::poll() {
    if (!send_handle_ || !conn_ || !conn_->tcp_client) {
        return -1;
    }

    auto* ctx = send_handle_->connection_ctx.get();
    const ConnectionParams& params = ctx->get_params();
    const uint32_t mtu = params.mtu_bytes;
    const uint16_t ppc = params.packets_per_chunk;

    auto send_packets_range = [&](uint32_t start_packet, uint32_t packet_count) {
        std::cout << "[SR][Sender] Retransmitting packets " << start_packet
                  << " .. " << (start_packet + packet_count - 1) << std::endl;
        int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_socket < 0) {
            std::cerr << "[SR][Sender] Failed to create UDP socket for retransmit: " << strerror(errno) << "\n";
            return;
        }
        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(params.udp_server_port);
        if (inet_pton(AF_INET, params.udp_server_ip, &server_addr.sin_addr) <= 0) {
            std::cerr << "[SR][Sender] Invalid server IP for retransmit: " << params.udp_server_ip << "\n";
            close(udp_socket);
            return;
        }

        const uint8_t* data = static_cast<const uint8_t*>(send_handle_->user_buffer);
        for (uint32_t i = 0; i < packet_count; ++i) {
            uint32_t packet_offset = start_packet + i;
            size_t data_offset = static_cast<size_t>(packet_offset) * mtu;
            if (data_offset >= send_handle_->buffer_size) break;
            size_t remaining = send_handle_->buffer_size - data_offset;
            size_t packet_data_len = std::min(static_cast<size_t>(mtu), remaining);
            if (packet_data_len > SDRPacket::MAX_PAYLOAD_SIZE) {
                packet_data_len = SDRPacket::MAX_PAYLOAD_SIZE;
            }

            SDRPacket* packet = SDRPacket::create_data_packet(
                params.transfer_id, send_handle_->msg_id, packet_offset,
                ppc, data + data_offset, packet_data_len);
            if (!packet) continue;
            packet->header.chunk_seq = packet->header.get_chunk_id();
            size_t total_packet_size = sizeof(SDRPacketHeader) + packet_data_len;
            packet->header.to_network_order();
            sendto(udp_socket, packet, total_packet_size, 0,
                   (struct sockaddr*)&server_addr, sizeof(server_addr));
            SDRPacket::destroy(packet);
        }
        close(udp_socket);
    };

    auto retransmit_range = [&](uint32_t start_chunk, uint32_t count) {
        uint32_t mtu_bytes = mtu == 0 ? SDRPacket::MAX_PAYLOAD_SIZE : mtu;
        uint64_t chunk_bytes = static_cast<uint64_t>(ppc) * mtu_bytes;
        uint64_t offset = static_cast<uint64_t>(start_chunk) * chunk_bytes;
        uint64_t length = static_cast<uint64_t>(count) * chunk_bytes;
        if (offset >= send_handle_->buffer_size) return;
        if (offset + length > send_handle_->buffer_size) {
            length = send_handle_->buffer_size - offset;
        }
        uint32_t start_packet = static_cast<uint32_t>(offset / mtu_bytes);
        uint32_t packet_count = static_cast<uint32_t>((length + mtu_bytes - 1) / mtu_bytes);
        send_packets_range(start_packet, packet_count);
        stats_.retransmits += count;
        auto now = std::chrono::steady_clock::now();
        for (uint32_t c = start_chunk; c < start_chunk + count && c < total_chunks_; ++c) {
            last_tx_[c] = now;
        }
    };
    auto retransmit_missing_from_bitmap = [&]() {
        uint32_t sent = 0;
        auto now = std::chrono::steady_clock::now();
        for (uint32_t c = 0; c < total_chunks_; ++c) {
            if (chunk_acked_[c]) continue;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tx_[c]).count();
            if (elapsed > static_cast<long>(cfg_.rto_ms)) {
                retransmit_range(c, 1);
                sent++;
                if (sent >= 8) break; // throttle burst
            }
        }
    };
    
    // Helper to apply bitmap from control message
    auto apply_bitmap = [&](const ControlMessage& msg) {
        uint32_t words = msg.chunk_bitmap_words;
        uint32_t max_chunks = total_chunks_;
        for (uint32_t w = 0; w < words && w < 4; ++w) {
            uint64_t word = msg.chunk_bitmap[w];
            for (uint32_t bit = 0; bit < 64; ++bit) {
                uint32_t chunk_id = w * 64 + bit;
                if (chunk_id >= max_chunks) break;
                if (word & (1ULL << bit)) {
                    chunk_acked_[chunk_id] = true;
                }
            }
        }
    };

    // Simple control loop: process SR_ACK/SR_NACK until COMPLETE or error
    while (true) {
        ControlMessage msg;
        if (!conn_->tcp_client->receive_message(msg)) {
            // Timeout: poll completion and drive RTO-based retransmits
            int rc = sdr_send_poll(send_handle_.get());
            if (rc == 0) return 0;
            auto now = std::chrono::steady_clock::now();
            for (uint32_t c = 0; c < total_chunks_; ++c) {
                if (chunk_acked_[c]) continue;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tx_[c]).count();
                if (elapsed > static_cast<long>(cfg_.rto_ms)) {
                    std::cout << "[SR][Sender] RTO retransmit chunk " << c << std::endl;
                    retransmit_range(c, 1);
                    last_tx_[c] = now;
                }
            }
            continue;
        }

        if (msg.msg_type == ControlMsgType::SR_ACK) {
            uint32_t cum_chunk = msg.params.max_inflight; // reused field
            std::cout << "[SR][Sender] Received SR_ACK cum=" << cum_chunk
                      << " total=" << msg.params.total_chunks << std::endl;
            apply_bitmap(msg);
            retransmit_missing_from_bitmap();
            if (cum_chunk + 1 >= msg.params.total_chunks) {
                return 0;
            }
            // keep waiting
        } else if (msg.msg_type == ControlMsgType::SR_NACK) {
            uint32_t start_chunk = msg.params.rto_ms;         // reused for start
            uint32_t missing_len = msg.params.rtt_alpha_ms;   // reused for length
            std::cout << "[SR][Sender] Received SR_NACK start=" << start_chunk
                      << " len=" << missing_len << std::endl;
            if (missing_len == 0) missing_len = 1;
            retransmit_range(start_chunk, missing_len);
            apply_bitmap(msg);
            retransmit_missing_from_bitmap();
        } else if (msg.msg_type == ControlMsgType::COMPLETE_ACK) {
            std::cout << "[SR][Sender] COMPLETE_ACK\n";
            return 0;
        } else if (msg.msg_type == ControlMsgType::INCOMPLETE_NACK) {
            return -1;
        } else {
            // Unknown message; check completion and continue
            int rc = sdr_send_poll(send_handle_.get());
            if (rc == 0) return 0;
        }
    }
}

int SRReceiver::post_receive(SDRConnection* conn, void* buffer, size_t length) {
    conn_ = conn;
    SDRRecvHandle* raw_handle = nullptr;
    int rc = sdr_recv_post(conn, buffer, length, &raw_handle);
    if (rc != 0) {
        std::cerr << "[SR] Failed to post receive\n";
        return rc;
    }
    recv_handle_.reset(raw_handle);
    return 0;
}

bool SRReceiver::pump() {
    if (!recv_handle_ || !conn_ || !conn_->tcp_server) {
        return false;
    }
    static auto last_ctrl = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto ctrl_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ctrl).count();
    if (ctrl_elapsed < static_cast<long>(std::max<uint32_t>(cfg_.nack_delay_ms, 100u))) {
        return false; // limit control emission rate
    }
    last_ctrl = now;
    const uint8_t* bitmap = nullptr;
    size_t len = 0;
    if (sdr_recv_bitmap_get(recv_handle_.get(), &bitmap, &len) != 0) {
        return false;
    }

    auto* ctx = recv_handle_->msg_ctx.get();
    if (!ctx || !ctx->frontend_bitmap) return false;
    uint32_t total_chunks = ctx->total_chunks;

    // Compute cumulative ack (highest contiguous chunk)
    uint32_t cumulative = 0;
    while (cumulative < total_chunks && ctx->frontend_bitmap->is_chunk_complete(cumulative)) {
        cumulative++;
    }
    if (cumulative > 0) cumulative -= 1; // last completed contiguous index

    // Build chunk bitmap snapshot (up to 4 words)
    uint64_t words[4] = {0,0,0,0};
    uint32_t word_count = (total_chunks + 63) / 64;
    if (word_count > 4) word_count = 4;
    for (uint32_t c = 0; c < total_chunks && c < 256; ++c) {
        if (ctx->frontend_bitmap->is_chunk_complete(c)) {
            uint32_t w = c / 64;
            uint32_t b = c % 64;
            if (w < 4) words[w] |= (1ULL << b);
        }
    }

    // Find first gap after cumulative
    uint32_t missing_start = 0;
    uint32_t missing_len = 0;
    for (uint32_t c = cumulative + 1; c < total_chunks; ++c) {
        if (!ctx->frontend_bitmap->is_chunk_complete(c)) {
            missing_start = c;
            // count run of missing
            uint32_t run = 0;
            while (c < total_chunks && !ctx->frontend_bitmap->is_chunk_complete(c)) {
                ++run; ++c;
            }
            missing_len = run;
            break;
        }
    }

    ControlMessage msg{};
    msg.magic = ControlMessage::MAGIC_VALUE;
    msg.connection_id = conn_->connection_ctx->get_connection_id();
    msg.params.total_chunks = static_cast<uint16_t>(total_chunks);
    msg.params.max_inflight = cumulative;
    msg.chunk_bitmap_words = static_cast<uint16_t>(word_count);
    for (uint32_t i = 0; i < word_count; ++i) {
        msg.chunk_bitmap[i] = words[i];
    }

    if (missing_len > 0) {
        msg.msg_type = ControlMsgType::SR_NACK;
        msg.params.rto_ms = missing_start;        // reuse for start chunk
        msg.params.rtt_alpha_ms = missing_len;    // reuse for length
        conn_->tcp_server->send_message(msg);
        std::cout << "[SR][Receiver] NACK start=" << missing_start
                  << " len=" << missing_len << std::endl;
        stats_.nacks_sent++;
    } else {
        if (ctx->frontend_bitmap->get_total_chunks_completed() >= total_chunks) {
            // All chunks complete: send completion and signal done
            msg.msg_type = ControlMsgType::COMPLETE_ACK;
            conn_->tcp_server->send_message(msg);
            std::cout << "[SR][Receiver] COMPLETE_ACK\n";
            stats_.acks_sent++;
            return true;
        } else {
            msg.msg_type = ControlMsgType::SR_ACK;
            conn_->tcp_server->send_message(msg);
            std::cout << "[SR][Receiver] ACK cum=" << cumulative << std::endl;
            stats_.acks_sent++;
        }
    }

    // Keep running until explicitly marked complete by COMPLETE_ACK
    return (ctx->frontend_bitmap->get_total_chunks_completed() >= total_chunks);
}

} // namespace sdr::reliability
