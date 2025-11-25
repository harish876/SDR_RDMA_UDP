#pragma once

#include "sdr_api.h"
#include "tcp_control.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace sdr::reliability {

struct ECConfig {
    uint16_t k_data{0};
    uint16_t m_parity{0};
    uint32_t fallback_timeout_ms{0};
    uint64_t data_bytes{0}; // original data length (without parity)
    uint32_t max_retries{3}; // max decode/retransmit attempts
};

struct ECStats {
    uint64_t parity_sent{0};
    uint64_t decode_success{0};
    uint64_t fallback_sr{0};
};

class ECSender {
public:
    explicit ECSender(const ECConfig& cfg) : cfg_(cfg) {}

    int encode_and_send(SDRConnection* conn, const void* buffer, size_t length);
    int poll();
    const ECStats& stats() const { return stats_; }

private:
    ECConfig cfg_;
    ECStats stats_{};
    std::vector<std::unique_ptr<SDRSendHandle, void(*)(SDRSendHandle*)>> sends_;
    std::vector<uint8_t> send_storage_; // holds data+parity buffer
    SDRConnection* conn_{nullptr};
    std::vector<bool> chunk_acked_;
    bool fallback_active_{false};
};

class ECReceiver {
public:
    explicit ECReceiver(const ECConfig& cfg) : cfg_(cfg) {}

    int post_receive(SDRConnection* conn, void* buffer, size_t length);
    bool try_decode();
    const ECStats& stats() const { return stats_; }
    SDRRecvHandle* handle() const { return recv_handle_.get(); }

private:
    ECConfig cfg_;
    ECStats stats_{};
    std::unique_ptr<SDRRecvHandle, void(*)(SDRRecvHandle*)> recv_handle_{nullptr, [](SDRRecvHandle* h){ delete h; }};
    SDRConnection* conn_{nullptr};

    // Receiver-side bookkeeping
    uint64_t data_bytes_{0};
    uint32_t chunk_bytes_{0};
    uint16_t k_{0};
    uint16_t m_{0};
    uint32_t data_chunks_{0};
    uint32_t parity_chunks_{0};
    uint32_t stripes_{0};
    uint32_t decode_attempts_{0};
    bool fallback_active_{false};
};

} // namespace sdr::reliability
