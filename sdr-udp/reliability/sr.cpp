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
    if (!send_handle_) {
        return -1;
    }
    // Placeholder: in a full implementation, we'd process ACK/NACK and retransmit missing chunks.
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
    if (!recv_handle_) {
        return false;
    }
    // Placeholder: a real implementation would emit ACK/NACK via TCP control as bitmap advances.
    const uint8_t* bitmap = nullptr;
    size_t len = 0;
    if (sdr_recv_bitmap_get(recv_handle_.get(), &bitmap, &len) != 0) {
        return false;
    }
    // Completion decision: rely on sdr_recv_complete externally.
    return true;
}

} // namespace sdr::reliability
