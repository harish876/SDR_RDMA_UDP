#pragma once

#include <cstdint>
#include <atomic>
#include <memory>

namespace sdr {

class BackendBitmap {
public:
    BackendBitmap(uint32_t total_packets, uint16_t packets_per_chunk);
    
    bool set_packet_received(uint32_t packet_offset);
    
    bool is_packet_received(uint32_t packet_offset) const;
    
    bool is_chunk_complete(uint32_t chunk_id) const;
    
    uint32_t get_chunk_packet_count(uint32_t chunk_id) const;
    
    uint32_t get_total_packets_received() const;
    
    // Get packet bitmap snapshot (for frontend polling)
    // Returns pointer to internal bitmap (read-only, thread-safe for reading)
    const std::atomic<uint64_t>* get_packet_bitmap() const {
        return packet_bitmap_.get();
    }
    
    uint32_t get_bitmap_size() const {
        return num_words_;
    }
    
    static void get_bit_position(uint32_t packet_offset, uint32_t& word_idx, uint32_t& bit_pos) {
        word_idx = packet_offset / 64;
        bit_pos = packet_offset % 64;
    }
    
    uint32_t get_total_packets() const { return total_packets_; }
    uint16_t get_packets_per_chunk() const { return packets_per_chunk_; }
    
private:
    std::unique_ptr<std::atomic<uint64_t>[]> packet_bitmap_;
    uint32_t num_words_;  // Number of uint64_t words in bitmap
    uint32_t total_packets_;
    uint16_t packets_per_chunk_;
    
    // Helper: check if all packets in chunk range are set
    bool check_chunk_range(uint32_t chunk_start_packet, uint32_t chunk_end_packet) const;
};

// Implementation
inline BackendBitmap::BackendBitmap(uint32_t total_packets, uint16_t packets_per_chunk)
    : total_packets_(total_packets), packets_per_chunk_(packets_per_chunk) {
    // Allocate enough uint64_t words to hold all packet bits
    // Each uint64_t holds 64 bits
    num_words_ = (total_packets + 63) / 64;
    
    // Allocate array of atomics (using default construction, then initialize)
    packet_bitmap_ = std::make_unique<std::atomic<uint64_t>[]>(num_words_);
    
    // Initialize all bits to 0 (atomically)
    for (uint32_t i = 0; i < num_words_; ++i) {
        packet_bitmap_[i].store(0, std::memory_order_relaxed);
    }
}

inline bool BackendBitmap::set_packet_received(uint32_t packet_offset) {
    if (packet_offset >= total_packets_) {
        return false;
    }
    
    uint32_t word_idx, bit_pos;
    get_bit_position(packet_offset, word_idx, bit_pos);
    
    // Atomically set the bit using fetch_or
    // Returns old value, so if bit was already set, old_value will have that bit set
    uint64_t bit_mask = 1ULL << bit_pos;
    uint64_t old_value = packet_bitmap_[word_idx].fetch_or(bit_mask, std::memory_order_release);
    
    // Return true if bit was newly set (wasn't set before)
    return (old_value & bit_mask) == 0;
}

inline bool BackendBitmap::is_packet_received(uint32_t packet_offset) const {
    if (packet_offset >= total_packets_) {
        return false;
    }
    
    uint32_t word_idx, bit_pos;
    get_bit_position(packet_offset, word_idx, bit_pos);
    
    uint64_t bit_mask = 1ULL << bit_pos;
    uint64_t value = packet_bitmap_[word_idx].load(std::memory_order_acquire);
    
    return (value & bit_mask) != 0;
}

inline bool BackendBitmap::is_chunk_complete(uint32_t chunk_id) const {
    uint32_t chunk_start_packet = chunk_id * packets_per_chunk_;
    uint32_t chunk_end_packet = chunk_start_packet + packets_per_chunk_;
    
    // Don't exceed total packets
    if (chunk_end_packet > total_packets_) {
        chunk_end_packet = total_packets_;
    }
    
    return check_chunk_range(chunk_start_packet, chunk_end_packet);
}

inline uint32_t BackendBitmap::get_chunk_packet_count(uint32_t chunk_id) const {
    uint32_t chunk_start_packet = chunk_id * packets_per_chunk_;
    uint32_t chunk_end_packet = chunk_start_packet + packets_per_chunk_;
    
    if (chunk_end_packet > total_packets_) {
        chunk_end_packet = total_packets_;
    }
    
    uint32_t count = 0;
    for (uint32_t i = chunk_start_packet; i < chunk_end_packet; ++i) {
        if (is_packet_received(i)) {
            count++;
        }
    }
    return count;
}

inline uint32_t BackendBitmap::get_total_packets_received() const {
    uint32_t count = 0;
    for (uint32_t i = 0; i < total_packets_; ++i) {
        if (is_packet_received(i)) {
            count++;
        }
    }
    return count;
}

inline bool BackendBitmap::check_chunk_range(uint32_t chunk_start_packet, uint32_t chunk_end_packet) const {
    // Check if all packets in the range are set
    // We can optimize by checking whole words when possible
    
    uint32_t start_word, start_bit;
    get_bit_position(chunk_start_packet, start_word, start_bit);
    
    uint32_t end_word, end_bit;
    get_bit_position(chunk_end_packet - 1, end_word, end_bit);
    end_bit++; // Make it exclusive
    
    // Check if all words between start and end have all bits set
    if (start_word == end_word) {
        // All packets in same word
        uint64_t mask = 0;
        for (uint32_t bit = start_bit; bit < end_bit; ++bit) {
            mask |= (1ULL << bit);
        }
        
        uint64_t value = packet_bitmap_[start_word].load(std::memory_order_acquire);
        return (value & mask) == mask;
    } else {
        // Span multiple words
        // Check first word (partial)
        uint64_t first_mask = 0;
        for (uint32_t bit = start_bit; bit < 64; ++bit) {
            first_mask |= (1ULL << bit);
        }
        uint64_t first_value = packet_bitmap_[start_word].load(std::memory_order_acquire);
        if ((first_value & first_mask) != first_mask) {
            return false;
        }
        
        // Check middle words (should be all 1s)
        for (uint32_t word = start_word + 1; word < end_word; ++word) {
            uint64_t value = packet_bitmap_[word].load(std::memory_order_acquire);
            if (value != UINT64_MAX) {
                return false;
            }
        }
        
        // Check last word (partial)
        uint64_t last_mask = 0;
        for (uint32_t bit = 0; bit < end_bit; ++bit) {
            last_mask |= (1ULL << bit);
        }
        uint64_t last_value = packet_bitmap_[end_word].load(std::memory_order_acquire);
        return (last_value & last_mask) == last_mask;
    }
}

} // namespace sdr


