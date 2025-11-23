#include "reliability/sr.h"
#include <iostream>

namespace sdr::reliability {

int SRSender::start_send(SDRConnection* conn, const void* buffer, size_t length) {
    conn_ = conn;
    SDRSendHandle* raw_handle = nullptr;
    int rc = sdr_send_post(conn, buffer, length, &raw_handle);
    if (rc != 0) {
        std::cerr << "[SR] Failed to start send\n";
        return rc;
    }
    send_handle_.reset(raw_handle);
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

    auto retransmit_range = [&](uint32_t start_chunk, uint32_t count) {
        uint32_t mtu_bytes = mtu == 0 ? SDRPacket::MAX_PAYLOAD_SIZE : mtu;
        uint64_t chunk_bytes = static_cast<uint64_t>(ppc) * mtu_bytes;
        uint64_t offset = static_cast<uint64_t>(start_chunk) * chunk_bytes;
        uint64_t length = static_cast<uint64_t>(count) * chunk_bytes;
        if (offset >= send_handle_->buffer_size) return;
        if (offset + length > send_handle_->buffer_size) {
            length = send_handle_->buffer_size - offset;
        }
        sdr_send_stream_continue(nullptr /*unused in stub*/, 0, 0); // placeholder to keep API visible
        // Use stream_continue via a temporary handle
        SDRStreamHandle* stream = nullptr;
        if (sdr_send_stream_start(conn_, send_handle_->user_buffer, send_handle_->buffer_size, 0, &stream) == 0) {
            sdr_send_stream_continue(stream, static_cast<uint32_t>(offset), static_cast<size_t>(length));
            sdr_send_stream_end(stream);
        }
    };

    // Wait for control messages from receiver
    ControlMessage msg;
    if (!conn_->tcp_client->receive_message(msg)) {
        return -1;
    }

    if (msg.msg_type == ControlMsgType::SR_ACK) {
        // If cumulative ack covers all chunks, we are done
        uint32_t cum_chunk = msg.params.max_inflight; // reused field
        if (cum_chunk + 1 >= msg.params.total_chunks) {
            return 0;
        }
    } else if (msg.msg_type == ControlMsgType::SR_NACK) {
        uint32_t start_chunk = msg.params.rto_ms;         // reused for start
        uint32_t missing_len = msg.params.rtt_alpha_ms;   // reused for length
        if (missing_len == 0) missing_len = 1;
        retransmit_range(start_chunk, missing_len);
    } else if (msg.msg_type == ControlMsgType::COMPLETE_ACK) {
        return 0;
    } else if (msg.msg_type == ControlMsgType::INCOMPLETE_NACK) {
        return -1;
    }

    // Fall back to polling completion if no SR messages indicate done.
    return sdr_send_poll(send_handle_.get());
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

    if (missing_len > 0) {
        msg.msg_type = ControlMsgType::SR_NACK;
        msg.params.rto_ms = missing_start;        // reuse for start chunk
        msg.params.rtt_alpha_ms = missing_len;    // reuse for length
        conn_->tcp_server->send_message(msg);
        stats_.nacks_sent++;
    } else {
        msg.msg_type = ControlMsgType::SR_ACK;
        conn_->tcp_server->send_message(msg);
        stats_.acks_sent++;
    }

    return (ctx->frontend_bitmap->get_total_chunks_completed() >= total_chunks);
}

} // namespace sdr::reliability
