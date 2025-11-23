#include "reliability/ec.h"
#include <iostream>

namespace sdr::reliability {

int ECSender::encode_and_send(SDRConnection* conn, const void* buffer, size_t length) {
    conn_ = conn;
    sends_.clear();
    // Placeholder: real implementation would split into data/parity submessages.
    SDRSendHandle* raw_handle = nullptr;
    int rc = sdr_send_post(conn, buffer, length, &raw_handle);
    if (rc != 0) {
        std::cerr << "[EC] Failed to send data buffer\n";
        return rc;
    }
    sends_.emplace_back(raw_handle, [](SDRSendHandle* h){ delete h; });
    return 0;
}

int ECSender::poll() {
    int rc = 0;
    for (auto& h : sends_) {
        rc = sdr_send_poll(h.get());
        if (rc != 0) {
            break;
        }
    }
    return rc;
}

int ECReceiver::post_receive(SDRConnection* conn, void* buffer, size_t length) {
    conn_ = conn;
    SDRRecvHandle* raw_handle = nullptr;
    int rc = sdr_recv_post(conn, buffer, length, &raw_handle);
    if (rc != 0) {
        std::cerr << "[EC] Failed to post receive\n";
        return rc;
    }
    recv_handle_.reset(raw_handle);
    return 0;
}

bool ECReceiver::try_decode() {
    if (!recv_handle_) {
        return false;
    }
    // Placeholder: integrate with EC decoder; currently always returns true when bitmap exists.
    const uint8_t* bitmap = nullptr;
    size_t len = 0;
    return sdr_recv_bitmap_get(recv_handle_.get(), &bitmap, &len) == 0;
}

} // namespace sdr::reliability
