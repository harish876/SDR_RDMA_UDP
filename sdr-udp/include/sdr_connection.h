#pragma once

#include "sdr_backend.h"
#include "sdr_frontend.h"
#include "tcp_control.h"
#include <cstdint>
#include <memory>
#include <array>
#include <mutex>
#include <atomic>
#include <cstring>
#include <vector>

namespace sdr {

// Message state
enum class MessageState : uint8_t {
    ACTIVE = 0,      // Message is active and receiving packets
    COMPLETED = 1,   // Message has been completed
    DEAD = 2,        // Message is completed and protected with null sink
    NULL_STATE = 3   // Message slot is null (unused)
};

// Message context (per message)
struct MessageContext {
    uint32_t msg_id;                // Message identifier (0-1023)
    uint32_t generation;             // Generation number (for late packet protection)
    MessageState state;              // Current state
    
    void* buffer;                    // User receive buffer
    size_t buffer_size;              // Buffer size in bytes
    size_t total_packets;            // Total packets in message
    size_t total_chunks;             // Total chunks in message
    uint16_t packets_per_chunk;      // Packets per chunk (P)
    
    // Bitmaps
    std::shared_ptr<BackendBitmap> backend_bitmap;
    std::shared_ptr<FrontendBitmap> frontend_bitmap;
    
    // Connection parameters
    ConnectionParams connection_params;
    
    MessageContext()
        : msg_id(0), generation(0), state(MessageState::NULL_STATE),
          buffer(nullptr), buffer_size(0), total_packets(0), total_chunks(0),
          packets_per_chunk(0) {
        memset(&connection_params, 0, sizeof(connection_params));
    }
};

// Connection context (per connection)
class ConnectionContext {
public:
    ConnectionContext();
    ~ConnectionContext();
    
    bool initialize(uint32_t connection_id, const ConnectionParams& params);
    
    MessageContext* allocate_message_slot(uint32_t msg_id, uint32_t generation);
    MessageContext* get_message(uint32_t msg_id) const;
    void release_message(uint32_t msg_id);

    void complete_message(uint32_t msg_id);
    
    void set_tcp_socket(int tcp_fd) { tcp_socket_fd_ = tcp_fd; }
    void set_udp_socket(int udp_fd) { udp_socket_fd_ = udp_fd; }
    int get_tcp_socket() const { return tcp_socket_fd_; }
    int get_udp_socket() const { return udp_socket_fd_; }
    
    uint32_t get_connection_id() const { return connection_id_; }
    const ConnectionParams& get_params() const { return params_; }
    
    bool is_initialized() const { return is_initialized_; }

    void set_auto_send_data(bool enable) { auto_send_data_ = enable; }
    bool auto_send_data() const { return auto_send_data_; }
    
    void calculate_bitmap_sizes(size_t total_bytes, uint32_t mtu_bytes,
                               uint16_t packets_per_chunk,
                               size_t& total_packets, size_t& total_chunks);
    
private:
    uint32_t connection_id_;
    ConnectionParams params_;
    bool is_initialized_;
    bool auto_send_data_;
    
    // Message table: fixed-size array indexed by msg_id (0-1023)
    static constexpr size_t MAX_MESSAGES = 1024;
    std::array<std::unique_ptr<MessageContext>, MAX_MESSAGES> msg_table_;
    mutable std::mutex msg_table_mutex_;  // Protects message table
    std::vector<uint8_t> null_sink_;      // Single-byte null sink for late packets
    std::array<uint32_t, MAX_MESSAGES> generation_counters_{}; // rotating generations per msg_id
    
    // Socket file descriptors
    int tcp_socket_fd_;
    int udp_socket_fd_;
};


inline ConnectionContext::ConnectionContext()
    : connection_id_(0), is_initialized_(false), auto_send_data_(true),
      tcp_socket_fd_(-1), udp_socket_fd_(-1) {
    memset(&params_, 0, sizeof(params_));
    null_sink_.resize(1, 0);
    generation_counters_.fill(1); // start generations at 1
}

inline ConnectionContext::~ConnectionContext() {
    std::lock_guard<std::mutex> lock(msg_table_mutex_);
    for (auto& msg : msg_table_) {
        msg.reset();
    }
}

inline bool ConnectionContext::initialize(uint32_t connection_id, const ConnectionParams& params) {
    std::lock_guard<std::mutex> lock(msg_table_mutex_);
    
    connection_id_ = connection_id;
    params_ = params;
    is_initialized_ = true;
    
    return true;
}

inline MessageContext* ConnectionContext::allocate_message_slot(uint32_t msg_id, uint32_t generation) {
    if (msg_id >= MAX_MESSAGES) {
        return nullptr;
    }
    
    std::lock_guard<std::mutex> lock(msg_table_mutex_);
    
    auto& msg_ptr = msg_table_[msg_id];
    // Allow reuse if slot is NULL or DEAD/COMPLETED with older generation.
    // Reject if slot is ACTIVE (transfer in progress) or generation is not newer.
    if (msg_ptr && msg_ptr->state == MessageState::ACTIVE) {
        return nullptr;
    }

    if (msg_ptr && (msg_ptr->state == MessageState::COMPLETED || msg_ptr->state == MessageState::DEAD)
        && msg_ptr->generation >= generation) {
        return nullptr;
    }
    
    msg_ptr = std::make_unique<MessageContext>();
    // Rotate generation to avoid late packet corruption
    uint32_t rotated_gen = generation_counters_[msg_id]++;
    msg_ptr->msg_id = msg_id;
    msg_ptr->generation = rotated_gen;
    msg_ptr->state = MessageState::ACTIVE;
    msg_ptr->connection_params = params_;
    
    return msg_ptr.get();
}

inline MessageContext* ConnectionContext::get_message(uint32_t msg_id) const {
    if (msg_id >= MAX_MESSAGES) {
        return nullptr;
    }
    
    std::lock_guard<std::mutex> lock(msg_table_mutex_);
    const auto& msg_ptr = msg_table_[msg_id];
    
    if (!msg_ptr || msg_ptr->state == MessageState::NULL_STATE) {
        return nullptr;
    }
    
    return msg_ptr.get();
}

inline void ConnectionContext::release_message(uint32_t msg_id) {
    if (msg_id >= MAX_MESSAGES) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(msg_table_mutex_);
    msg_table_[msg_id].reset();
}

inline void ConnectionContext::complete_message(uint32_t msg_id) {
    if (msg_id >= MAX_MESSAGES) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(msg_table_mutex_);
    auto& msg_ptr = msg_table_[msg_id];
    
    if (msg_ptr) {
        msg_ptr->state = MessageState::DEAD;
        msg_ptr->buffer = null_sink_.data(); // Redirect to null sink to avoid late-packet corruption
    }
}

inline void ConnectionContext::calculate_bitmap_sizes(size_t total_bytes, uint32_t mtu_bytes,
                                                      uint16_t packets_per_chunk,
                                                      size_t& total_packets, size_t& total_chunks) {
    if (mtu_bytes == 0 || packets_per_chunk == 0) {
        total_packets = 0;
        total_chunks = 0;
        return;
    }
    
    total_packets = (total_bytes + mtu_bytes - 1) / mtu_bytes;
    
    // Calculate total chunks (ceiling division)
    // Formula: (n + k - 1) / k gives ceiling(n/k) when using integer division
    total_chunks = (total_packets + packets_per_chunk - 1) / packets_per_chunk;
}

} // namespace sdr
