#pragma once

#include "sdr_backend.h"
#include "sdr_frontend.h"
#include "tcp_control.h"
#include <cstdint>
#include <memory>
#include <array>
#include <mutex>
#include <atomic>

namespace sdr {

// Message state
enum class MessageState : uint8_t {
    ACTIVE = 0,      // Message is active and receiving packets
    COMPLETED = 1,   // Message has been completed
    NULL_STATE = 2   // Message slot is null (for late packet protection)
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
        std::memset(&connection_params, 0, sizeof(connection_params));
    }
};

// Connection context (per connection)
class ConnectionContext {
public:
    ConnectionContext();
    ~ConnectionContext();
    
    // Initialize connection with parameters
    bool initialize(uint32_t connection_id, const ConnectionParams& params);
    
    // Message table management
    MessageContext* allocate_message_slot(uint32_t msg_id, uint32_t generation);
    MessageContext* get_message(uint32_t msg_id) const;
    void release_message(uint32_t msg_id);
    
    // Mark message as completed (for late packet protection)
    void complete_message(uint32_t msg_id);
    
    // TCP/UDP socket management
    void set_tcp_socket(int tcp_fd) { tcp_socket_fd_ = tcp_fd; }
    void set_udp_socket(int udp_fd) { udp_socket_fd_ = udp_fd; }
    int get_tcp_socket() const { return tcp_socket_fd_; }
    int get_udp_socket() const { return udp_socket_fd_; }
    
    uint32_t get_connection_id() const { return connection_id_; }
    const ConnectionParams& get_params() const { return params_; }
    
    // Check if connection is initialized
    bool is_initialized() const { return is_initialized_; }
    
    // Helper: calculate bitmap sizes from message parameters
    void calculate_bitmap_sizes(size_t total_bytes, uint32_t mtu_bytes,
                               uint16_t packets_per_chunk,
                               size_t& total_packets, size_t& total_chunks);
    
private:
    uint32_t connection_id_;
    ConnectionParams params_;
    bool is_initialized_;
    
    // Message table: fixed-size array indexed by msg_id (0-1023)
    static constexpr size_t MAX_MESSAGES = 1024;
    std::array<std::unique_ptr<MessageContext>, MAX_MESSAGES> msg_table_;
    mutable std::mutex msg_table_mutex_;  // Protects message table
    
    // Socket file descriptors
    int tcp_socket_fd_;
    int udp_socket_fd_;
};

// Implementation
inline ConnectionContext::ConnectionContext()
    : connection_id_(0), is_initialized_(false),
      tcp_socket_fd_(-1), udp_socket_fd_(-1) {
    std::memset(&params_, 0, sizeof(params_));
}

inline ConnectionContext::~ConnectionContext() {
    // Release all messages
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
    if (msg_ptr && msg_ptr->state != MessageState::NULL_STATE) {
        // Slot already in use (could be late packet from previous generation)
        return nullptr;
    }
    
    // Allocate new message context
    msg_ptr = std::make_unique<MessageContext>();
    msg_ptr->msg_id = msg_id;
    msg_ptr->generation = generation;
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
        msg_ptr->state = MessageState::COMPLETED;
        // Note: In production, we'd set buffer to NULL memory key here
        // For UDP, we just mark as completed
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
    
    // Calculate total packets (ceiling division)
    total_packets = (total_bytes + mtu_bytes - 1) / mtu_bytes;
    
    // Calculate total chunks (ceiling division)
    // Formula: (n + k - 1) / k gives ceiling(n/k) when using integer division
    total_chunks = (total_packets + packets_per_chunk - 1) / packets_per_chunk;
}

} // namespace sdr


