#pragma once

#include "sdr_backend.h"
#include <cstdint>
#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace sdr {

// Frontend chunk bitmap manager
// Polls packet bitmap from backend and updates chunk bitmap
class FrontendBitmap {
public:
    FrontendBitmap(std::shared_ptr<BackendBitmap> backend_bitmap, 
                   uint32_t total_chunks);
    
    ~FrontendBitmap();
    
    // Start the polling thread
    bool start_polling(uint32_t poll_interval_us = 100); // Default 100 microseconds
    
    // Stop the polling thread
    void stop_polling();
    
    // Check if chunk is complete (thread-safe)
    bool is_chunk_complete(uint32_t chunk_id) const;
    
    // Get chunk bitmap snapshot (thread-safe for reading)
    const std::atomic<uint64_t>* get_chunk_bitmap() const {
        return chunk_bitmap_.get();
    }
    
    // Get number of words in the chunk bitmap
    uint32_t get_chunk_bitmap_size() const {
        return num_words_;
    }
    
    // Get total chunks completed
    uint32_t get_total_chunks_completed() const;
    
    // Force a polling cycle (for testing/debugging)
    void poll_once();
    
    uint32_t get_total_chunks() const { return total_chunks_; }
    
private:
    std::shared_ptr<BackendBitmap> backend_bitmap_;
    std::unique_ptr<std::atomic<uint64_t>[]> chunk_bitmap_;
    uint32_t num_words_;  // Number of uint64_t words in bitmap
    uint32_t total_chunks_;
    uint16_t packets_per_chunk_;
    
    // Polling thread control
    std::thread poller_thread_;
    std::atomic<bool> should_stop_;
    std::mutex poller_mutex_;
    std::condition_variable poller_cv_;
    uint32_t poll_interval_us_;
    
    // Polling thread function
    void polling_thread_func();
    
    // Update chunk bitmap based on packet bitmap
    void update_chunk_bitmap();
    
    // Helper: check if chunk should be marked complete
    bool check_and_set_chunk(uint32_t chunk_id);
};

// Implementation
inline FrontendBitmap::FrontendBitmap(std::shared_ptr<BackendBitmap> backend_bitmap,
                                     uint32_t total_chunks)
    : backend_bitmap_(backend_bitmap),
      total_chunks_(total_chunks),
      packets_per_chunk_(backend_bitmap ? backend_bitmap->get_packets_per_chunk() : 0),
      should_stop_(false),
      poll_interval_us_(100) {
    
    // Allocate enough uint64_t words to hold all chunk bits
    num_words_ = (total_chunks + 63) / 64;
    
    // Allocate array of atomics (using default construction, then initialize)
    chunk_bitmap_ = std::make_unique<std::atomic<uint64_t>[]>(num_words_);
    
    // Initialize all bits to 0
    for (uint32_t i = 0; i < num_words_; ++i) {
        chunk_bitmap_[i].store(0, std::memory_order_relaxed);
    }
}

inline FrontendBitmap::~FrontendBitmap() {
    stop_polling();
}

inline bool FrontendBitmap::start_polling(uint32_t poll_interval_us) {
    if (poller_thread_.joinable()) {
        return false; // Already polling
    }
    
    poll_interval_us_ = poll_interval_us;
    should_stop_.store(false, std::memory_order_relaxed);
    
    poller_thread_ = std::thread(&FrontendBitmap::polling_thread_func, this);
    return true;
}

inline void FrontendBitmap::stop_polling() {
    if (!poller_thread_.joinable()) {
        return;
    }
    
    should_stop_.store(true, std::memory_order_relaxed);
    poller_cv_.notify_all();
    
    poller_thread_.join();
}

inline bool FrontendBitmap::is_chunk_complete(uint32_t chunk_id) const {
    if (chunk_id >= total_chunks_) {
        return false;
    }
    
    uint32_t word_idx = chunk_id / 64;
    uint32_t bit_pos = chunk_id % 64;
    
    uint64_t bit_mask = 1ULL << bit_pos;
    uint64_t value = chunk_bitmap_[word_idx].load(std::memory_order_acquire);
    
    return (value & bit_mask) != 0;
}

inline uint32_t FrontendBitmap::get_total_chunks_completed() const {
    uint32_t count = 0;
    for (uint32_t i = 0; i < total_chunks_; ++i) {
        if (is_chunk_complete(i)) {
            count++;
        }
    }
    return count;
}

inline void FrontendBitmap::poll_once() {
    update_chunk_bitmap();
}

inline void FrontendBitmap::polling_thread_func() {
    while (!should_stop_.load(std::memory_order_acquire)) {
        // Update chunk bitmap
        update_chunk_bitmap();
        
        // Sleep for poll interval
        std::unique_lock<std::mutex> lock(poller_mutex_);
        poller_cv_.wait_for(lock, 
                           std::chrono::microseconds(poll_interval_us_),
                           [this] { return should_stop_.load(std::memory_order_acquire); });
    }
}

inline void FrontendBitmap::update_chunk_bitmap() {
    if (!backend_bitmap_) {
        return;
    }
    
    // Check each chunk and update bitmap if complete
    for (uint32_t chunk_id = 0; chunk_id < total_chunks_; ++chunk_id) {
        check_and_set_chunk(chunk_id);
    }
}

inline bool FrontendBitmap::check_and_set_chunk(uint32_t chunk_id) {
    // Check if chunk is already marked complete
    if (is_chunk_complete(chunk_id)) {
        return true;
    }
    
    // Check if chunk is complete in backend packet bitmap
    if (!backend_bitmap_->is_chunk_complete(chunk_id)) {
        return false;
    }
    
    // Mark chunk as complete in chunk bitmap (atomically)
    uint32_t word_idx = chunk_id / 64;
    uint32_t bit_pos = chunk_id % 64;
    uint64_t bit_mask = 1ULL << bit_pos;
    
    uint64_t old_value = chunk_bitmap_[word_idx].fetch_or(bit_mask, std::memory_order_release);
    
    // Return true if we just set it (wasn't set before)
    return (old_value & bit_mask) == 0;
}

} // namespace sdr


