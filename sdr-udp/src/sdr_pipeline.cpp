#include "sdr_pipeline.h"
#include <random>
#include <chrono>

namespace sdr {

NetworkSimulator::NetworkSimulator(double drop_prob, uint32_t delay_ms, uint32_t jitter_ms)
    : drop_prob_(drop_prob), delay_ms_(delay_ms), jitter_ms_(jitter_ms) {}

bool NetworkSimulator::should_drop() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::bernoulli_distribution dist(drop_prob_);
    return dist(rng);
}

uint32_t NetworkSimulator::compute_delay_ms() {
    if (jitter_ms_ == 0) return delay_ms_;
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, jitter_ms_);
    return delay_ms_ + dist(rng);
}

void BitmapEngine::register_message(uint32_t msg_id, uint32_t total_packets, uint16_t packets_per_chunk,
                                    const ReliabilityCallbacks& cb) {
    std::lock_guard<std::mutex> lock(mu_);
    MsgState st;
    st.total_packets = total_packets;
    st.ppc = packets_per_chunk;
    st.total_chunks = (total_packets + packets_per_chunk - 1) / packets_per_chunk;
    st.packet_bitmap.assign((total_packets + 63) / 64, 0);
    st.chunk_bitmap.assign((st.total_chunks + 63) / 64, 0);
    st.callbacks = cb;
    msgs_[msg_id] = std::move(st);
}

void BitmapEngine::process_packet(const SDRPacketHeader& header) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = msgs_.find(header.msg_id);
    if (it == msgs_.end()) return;
    auto& st = it->second;
    uint32_t pkt = header.packet_offset;
    if (pkt >= st.total_packets) return;
    if (st.callbacks.on_packet && !st.callbacks.on_packet(header.msg_id, pkt)) {
        return;
    }
    uint32_t w = pkt / 64;
    uint32_t b = pkt % 64;
    st.packet_bitmap[w] |= (1ULL << b);

    uint32_t chunk = pkt / st.ppc;
    uint32_t chunk_word = chunk / 64;
    uint32_t chunk_bit = chunk % 64;

    // Check if all packets in chunk are set
    uint32_t start_pkt = chunk * st.ppc;
    uint32_t end_pkt = std::min<uint32_t>(start_pkt + st.ppc, st.total_packets);
    bool complete = true;
    for (uint32_t p = start_pkt; p < end_pkt; ++p) {
        uint32_t cw = p / 64;
        uint32_t cb = p % 64;
        if ((st.packet_bitmap[cw] & (1ULL << cb)) == 0) {
            complete = false;
            break;
        }
    }
    if (complete) {
        if ((st.chunk_bitmap[chunk_word] & (1ULL << chunk_bit)) == 0) {
            st.chunk_bitmap[chunk_word] |= (1ULL << chunk_bit);
            if (st.callbacks.on_chunk_complete) {
                st.callbacks.on_chunk_complete(header.msg_id, chunk);
            }
        }
    }
    // Message complete?
    bool msg_done = true;
    for (uint32_t c = 0; c < st.total_chunks; ++c) {
        uint32_t cw = c / 64;
        uint32_t cb = c % 64;
        if ((st.chunk_bitmap[cw] & (1ULL << cb)) == 0) {
            msg_done = false;
            break;
        }
    }
    if (msg_done && st.callbacks.on_message_complete) {
        st.callbacks.on_message_complete(header.msg_id);
    }
}

MultiChannelBackend::MultiChannelBackend(std::shared_ptr<BitmapEngine> engine, size_t channels)
    : engine_(std::move(engine)) {
    chans_.resize(channels);
    for (size_t i = 0; i < channels; ++i) {
        chans_[i] = std::make_unique<Chan>();
        chans_[i]->th = std::thread(&MultiChannelBackend::worker_loop, this, i);
    }
}

MultiChannelBackend::~MultiChannelBackend() {
    for (auto& c : chans_) {
        {
            std::lock_guard<std::mutex> lock(c->mu);
            c->stop = true;
        }
        c->cv.notify_all();
    }
    for (auto& c : chans_) {
        if (c->th.joinable()) c->th.join();
    }
}

void MultiChannelBackend::enqueue(const SDRPacketHeader& header) {
    size_t idx = rr_.fetch_add(1, std::memory_order_relaxed) % chans_.size();
    auto& chan = chans_[idx];
    {
        std::lock_guard<std::mutex> lock(chan->mu);
        chan->q.push(header);
    }
    chan->cv.notify_one();
}

void MultiChannelBackend::worker_loop(size_t idx) {
    auto& chan = chans_[idx];
    while (true) {
        SDRPacketHeader hdr;
        {
            std::unique_lock<std::mutex> lock(chan->mu);
            chan->cv.wait(lock, [&]{ return chan->stop || !chan->q.empty(); });
            if (chan->stop && chan->q.empty()) break;
            hdr = chan->q.front();
            chan->q.pop();
        }
        if (engine_) {
            engine_->process_packet(hdr);
        }
    }
}

SDRPipeline::SDRPipeline(size_t channels)
    : callbacks_{}, net_(std::make_shared<NetworkSimulator>()),
      engine_(std::make_shared<BitmapEngine>()),
      backend_(std::make_unique<MultiChannelBackend>(engine_, channels)) {}

void SDRPipeline::register_message(uint32_t msg_id, uint32_t total_packets, uint16_t ppc) {
    engine_->register_message(msg_id, total_packets, ppc, callbacks_);
}

bool SDRPipeline::submit_packet(const SDRPacketHeader& header) {
    if (net_ && net_->should_drop()) {
        return false;
    }
    uint32_t delay = net_ ? net_->compute_delay_ms() : 0;
    if (delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    backend_->enqueue(header);
    return true;
}

void SDRPipeline::configure_net(double drop_prob, uint32_t delay_ms, uint32_t jitter_ms) {
    net_->set_drop_prob(drop_prob);
    net_->set_delay(delay_ms, jitter_ms);
}

} // namespace sdr
