#pragma once

#include "sdr_packet.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace sdr {

struct ReliabilityCallbacks {
    std::function<bool(uint32_t /*msg_id*/, uint32_t /*packet_offset*/)> on_packet;
    std::function<void(uint32_t /*msg_id*/, uint32_t /*chunk_idx*/)> on_chunk_complete;
    std::function<void(uint32_t /*msg_id*/)> on_message_complete;
};

// Simple network simulator: drop/delay packets before backend.
class NetworkSimulator {
public:
    NetworkSimulator(double drop_prob = 0.0, uint32_t delay_ms = 0, uint32_t jitter_ms = 0);
    bool should_drop();
    uint32_t compute_delay_ms();

    void set_drop_prob(double p) { drop_prob_ = p; }
    void set_delay(uint32_t d, uint32_t j) { delay_ms_ = d; jitter_ms_ = j; }

private:
    double drop_prob_;
    uint32_t delay_ms_;
    uint32_t jitter_ms_;
};

struct SimPacket {
    SDRPacketHeader header;
};

class BitmapEngine {
public:
    void register_message(uint32_t msg_id, uint32_t total_packets, uint16_t packets_per_chunk,
                          const ReliabilityCallbacks& cb);
    void process_packet(const SDRPacketHeader& header);

private:
    struct MsgState {
        uint32_t total_packets{0};
        uint16_t ppc{0};
        uint32_t total_chunks{0};
        std::vector<uint64_t> packet_bitmap;
        std::vector<uint64_t> chunk_bitmap;
        ReliabilityCallbacks callbacks;
    };
    std::mutex mu_;
    std::map<uint32_t, MsgState> msgs_;
};

class MultiChannelBackend {
public:
    MultiChannelBackend(std::shared_ptr<BitmapEngine> engine, size_t channels);
    ~MultiChannelBackend();
    void enqueue(const SDRPacketHeader& header);

private:
    void worker_loop(size_t idx);
    std::shared_ptr<BitmapEngine> engine_;
    struct Chan {
        std::thread th;
        std::queue<SDRPacketHeader> q;
        std::mutex mu;
        std::condition_variable cv;
        bool stop{false};
    };
    std::vector<std::unique_ptr<Chan>> chans_;
    std::atomic<size_t> rr_{0};
};

class SDRPipeline {
public:
    SDRPipeline(size_t channels = 1);
    void set_callbacks(const ReliabilityCallbacks& cb) { callbacks_ = cb; }
    void register_message(uint32_t msg_id, uint32_t total_packets, uint16_t ppc);
    // Returns true if the packet was accepted into the pipeline (not dropped).
    bool submit_packet(const SDRPacketHeader& header);
    void configure_net(double drop_prob, uint32_t delay_ms, uint32_t jitter_ms);

private:
    ReliabilityCallbacks callbacks_;
    std::shared_ptr<NetworkSimulator> net_;
    std::shared_ptr<BitmapEngine> engine_;
    std::unique_ptr<MultiChannelBackend> backend_;
};

} // namespace sdr
