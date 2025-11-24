#pragma once

#include "sdr_api.h"
#include "tcp_control.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <chrono>

namespace sdr::reliability {

struct SRConfig {
    uint32_t rto_ms{0};            // Base RTO (RTT + alpha*RTT)
    uint32_t nack_delay_ms{0};     // Delay before emitting NACK
    uint16_t max_inflight_chunks{0};
    uint32_t base_rtt_ms{100};     // Estimated RTT
    uint32_t alpha_ms{100};        // RTT margin
};

struct SRStats {
    uint64_t acks_sent{0};
    uint64_t nacks_sent{0};
    uint64_t retransmits{0};
};

// Sender-side SR controller
class SRSender {
public:
    explicit SRSender(const SRConfig& cfg) : cfg_(cfg) {}

    // Start a reliable send. Owns the handle until completion.
    int start_send(SDRConnection* conn, const void* buffer, size_t length);

    // Drive progress; returns 0 on completion, <0 on error, >0 if still in progress.
    int poll();

    const SRStats& stats() const { return stats_; }

private:
    SRConfig cfg_;
    SRStats stats_{};
    uint32_t total_chunks_{0};
    uint32_t mtu_bytes_{0};
    uint16_t packets_per_chunk_{0};
    std::vector<bool> chunk_acked_;
    std::vector<std::chrono::steady_clock::time_point> last_tx_;
    std::chrono::steady_clock::time_point last_control_tx_{};
    std::unique_ptr<SDRSendHandle, void(*)(SDRSendHandle*)> send_handle_{nullptr, [](SDRSendHandle* h){ delete h; }};
    SDRConnection* conn_{nullptr};

    // Internal helpers would go here (timer management, retransmit queue, etc.).
};

// Receiver-side SR controller
class SRReceiver {
public:
    explicit SRReceiver(const SRConfig& cfg) : cfg_(cfg) {}

    // Post receive buffer and start tracking bitmap.
    int post_receive(SDRConnection* conn, void* buffer, size_t length);

    // Pump bitmap/ACK generation; returns true when complete.
    bool pump();

    const SRStats& stats() const { return stats_; }
    SDRRecvHandle* handle() const { return recv_handle_.get(); }

private:
    SRConfig cfg_;
    SRStats stats_{};
    std::unique_ptr<SDRRecvHandle, void(*)(SDRRecvHandle*)> recv_handle_{nullptr, [](SDRRecvHandle* h){ delete h; }};
    SDRConnection* conn_{nullptr};
    std::vector<bool> chunk_done_;
    uint32_t total_chunks_{0};

    void emit_control();
    bool handle_packet(uint32_t msg_id, uint32_t packet);
    void handle_chunk_complete(uint32_t msg_id, uint32_t chunk);
    void handle_message_complete(uint32_t msg_id);
};

} // namespace sdr::reliability
