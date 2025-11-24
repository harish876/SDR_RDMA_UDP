#include "reliability/sr.h"
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>
#include <algorithm>

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
    const uint32_t effective_rto_ms = cfg_.rto_ms ? cfg_.rto_ms : (cfg_.base_rtt_ms + cfg_.alpha_ms);
    const uint32_t guard_ms = 50; // suppress back-to-back retransmits

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
    auto retransmit_missing_from_bitmap = [&](uint32_t limit) {
        uint32_t sent = 0;
        auto now = std::chrono::steady_clock::now();
        for (uint32_t c = 0; c < total_chunks_ && sent < limit; ++c) {
            if (chunk_acked_[c]) continue;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tx_[c]).count();
            if (elapsed < static_cast<long>(guard_ms)) continue; // recently retransmitted
            retransmit_range(c, 1);
            sent++;
        }
    };
    
    // Helper to apply bitmap from control message
    auto apply_bitmap = [&](const ControlMessage& msg) {
        uint32_t words = msg.chunk_bitmap_words;
        uint32_t max_chunks = total_chunks_;
        for (uint32_t w = 0; w < words && w < 8; ++w) {
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
                if (elapsed > static_cast<long>(effective_rto_ms)) {
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
            stats_.acks_sent++;
            retransmit_missing_from_bitmap(4); // send a few missing chunks per control tick
            if (cum_chunk + 1 >= msg.params.total_chunks) {
                return 0;
            }
            // keep waiting
        } else if (msg.msg_type == ControlMsgType::SR_NACK) {
            uint32_t start_chunk = msg.params.rto_ms;         // reused for start
            uint32_t missing_len = msg.params.rtt_alpha_ms;   // reused for length
            std::cout << "[SR][Sender] Received SR_NACK start=" << start_chunk
                      << " len=" << missing_len << std::endl;
            stats_.nacks_sent++;
            apply_bitmap(msg);
            // retransmit missing chunks based on bitmap state, throttled
            // walk reported gaps
            uint32_t gap_limit = 8;
            uint32_t sent = 0;
            for (uint16_t i = 0; i < msg.num_gaps && sent < gap_limit; ++i) {
                uint32_t gs = msg.gap_start[i];
                uint32_t gl = msg.gap_len[i];
                uint32_t endc = std::min<uint32_t>(gs + gl, total_chunks_);
                for (uint32_t c = gs; c < endc && sent < gap_limit; ++c) {
                    if (chunk_acked_[c]) continue;
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tx_[c]).count();
                    if (elapsed < static_cast<long>(guard_ms)) continue; // recently retransmitted
                    retransmit_range(c, 1);
                    sent++;
                }
            }
            retransmit_missing_from_bitmap(4);
        } else if (msg.msg_type == ControlMsgType::COMPLETE_ACK) {
            std::cout << "[SR][Sender] COMPLETE_ACK\n";
            stats_.acks_sent++;
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
    ReliabilityCallbacks cbs{};
    cbs.on_packet = [this](uint32_t msg_id, uint32_t packet) { return handle_packet(msg_id, packet); };
    cbs.on_chunk_complete = [this](uint32_t msg_id, uint32_t chunk) { handle_chunk_complete(msg_id, chunk); };
    cbs.on_message_complete = [this](uint32_t msg_id) { handle_message_complete(msg_id); };
    conn_->connection_ctx->set_reliability_callbacks(cbs);

    SDRRecvHandle* raw_handle = nullptr;
    int rc = sdr_recv_post(conn, buffer, length, &raw_handle);
    if (rc != 0) {
        std::cerr << "[SR] Failed to post receive\n";
        return rc;
    }
    recv_handle_.reset(raw_handle);
    if (recv_handle_->msg_ctx) {
        total_chunks_ = static_cast<uint32_t>(recv_handle_->msg_ctx->total_chunks);
        chunk_done_.assign(total_chunks_, false);
    }
    return 0;
}

bool SRReceiver::pump() {
    // Callback-driven; pump now just emits based on current chunk_done_ state.
    if (!recv_handle_ || !conn_ || !conn_->tcp_server) {
        return false;
    }
    emit_control();
    return std::all_of(chunk_done_.begin(), chunk_done_.end(), [](bool v){ return v; });
}

bool SRReceiver::handle_packet(uint32_t msg_id, uint32_t packet) {
    if (!recv_handle_ || !recv_handle_->msg_ctx || recv_handle_->msg_ctx->msg_id != msg_id) {
        return false;
    }
    if (recv_handle_->msg_ctx->backend_bitmap) {
        recv_handle_->msg_ctx->backend_bitmap->set_packet_received(packet);
    }
    return true;
}

void SRReceiver::handle_chunk_complete(uint32_t msg_id, uint32_t chunk) {
    if (msg_id != (recv_handle_ && recv_handle_->msg_ctx ? recv_handle_->msg_ctx->msg_id : UINT32_MAX)) {
        return;
    }
    if (chunk < chunk_done_.size()) {
        chunk_done_[chunk] = true;
    }
    if (recv_handle_ && recv_handle_->msg_ctx && recv_handle_->msg_ctx->frontend_bitmap) {
        recv_handle_->msg_ctx->frontend_bitmap->poll_once();
    }
    emit_control();
}

void SRReceiver::handle_message_complete(uint32_t msg_id) {
    if (!recv_handle_ || !recv_handle_->msg_ctx || recv_handle_->msg_ctx->msg_id != msg_id) return;
    if (conn_ && conn_->tcp_server) {
        ControlMessage msg{};
        msg.magic = ControlMessage::MAGIC_VALUE;
        msg.msg_type = ControlMsgType::COMPLETE_ACK;
        msg.connection_id = conn_->connection_ctx->get_connection_id();
        conn_->tcp_server->send_message(msg);
        stats_.acks_sent++;
    }
}

void SRReceiver::emit_control() {
    if (!conn_ || !conn_->tcp_server || chunk_done_.empty()) return;
    uint32_t total_chunks = total_chunks_;
    uint32_t cumulative = 0;
    while (cumulative < total_chunks && chunk_done_[cumulative]) cumulative++;
    if (cumulative > 0) cumulative -= 1;

    uint64_t words[8] = {0,0,0,0,0,0,0,0};
    uint32_t word_count = (total_chunks + 63) / 64;
    if (word_count > 8) word_count = 8;
    for (uint32_t c = 0; c < total_chunks && c < 512; ++c) {
        if (chunk_done_[c]) {
            uint32_t w = c / 64;
            uint32_t b = c % 64;
            words[w] |= (1ULL << b);
        }
    }

    ControlMessage msg{};
    msg.magic = ControlMessage::MAGIC_VALUE;
    msg.connection_id = conn_->connection_ctx->get_connection_id();
    msg.params.total_chunks = static_cast<uint16_t>(total_chunks);
    msg.params.max_inflight = cumulative;
    msg.chunk_bitmap_words = static_cast<uint16_t>(word_count);
    for (uint32_t i = 0; i < word_count; ++i) msg.chunk_bitmap[i] = words[i];

    uint16_t gaps_found = 0;
    for (uint32_t c = cumulative + 1; c < total_chunks && gaps_found < 4; ++c) {
        if (!chunk_done_[c]) {
            uint16_t start = static_cast<uint16_t>(c);
            uint16_t len = 0;
            while (c < total_chunks && !chunk_done_[c] && len < UINT16_MAX) {
                ++len; ++c;
            }
            msg.gap_start[gaps_found] = start;
            msg.gap_len[gaps_found] = len;
            gaps_found++;
        }
    }
    msg.num_gaps = gaps_found;

    if (std::all_of(chunk_done_.begin(), chunk_done_.end(), [](bool v){ return v; })) {
        msg.msg_type = ControlMsgType::COMPLETE_ACK;
        conn_->tcp_server->send_message(msg);
        stats_.acks_sent++;
        return;
    }

    if (gaps_found > 0) {
        msg.msg_type = ControlMsgType::SR_NACK;
        msg.params.rto_ms = msg.gap_start[0];
        msg.params.rtt_alpha_ms = msg.gap_len[0];
        conn_->tcp_server->send_message(msg);
        stats_.nacks_sent++;
        std::cout << "[SR][Receiver] NACK start=" << msg.gap_start[0]
                  << " len=" << msg.gap_len[0] << std::endl;
    } else {
        msg.msg_type = ControlMsgType::SR_ACK;
        conn_->tcp_server->send_message(msg);
        stats_.acks_sent++;
        std::cout << "[SR][Receiver] ACK cum=" << cumulative << std::endl;
    }
}
} // namespace sdr::reliability
