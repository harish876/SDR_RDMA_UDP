#pragma once

#include "sdr_packet.h"
#include "sdr_connection.h"
#include "sdr_backend.h"
#include <cstdint>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <iostream>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>

namespace sdr {

// UDP receiver worker thread
// Receives packets and updates backend packet bitmaps
class UDPReceiver {
public:
    UDPReceiver(std::shared_ptr<ConnectionContext> connection);
    ~UDPReceiver();
    
    // Start receiver thread
    bool start(uint16_t udp_port);
    
    // Stop receiver thread
    void stop();
    
    // Check if receiver is running
    bool is_running() const { return is_running_.load(); }
    
private:
    std::shared_ptr<ConnectionContext> connection_;
    std::thread receiver_thread_;
    std::atomic<bool> should_stop_;
    std::atomic<bool> is_running_;
    int udp_socket_fd_;
    uint16_t udp_port_;
    
    // Receiver thread function
    void receiver_thread_func();
    
    // Process incoming packet
    void process_packet(const SDRPacketHeader& header, const uint8_t* payload, size_t payload_len);
    
    // Write packet payload to buffer
    void write_packet_to_buffer(MessageContext* msg_ctx, uint32_t packet_offset,
                                const uint8_t* payload, size_t payload_len);
};

// Implementation
inline UDPReceiver::UDPReceiver(std::shared_ptr<ConnectionContext> connection)
    : connection_(connection), should_stop_(false), is_running_(false),
      udp_socket_fd_(-1), udp_port_(0) {
}

inline UDPReceiver::~UDPReceiver() {
    stop();
}

inline bool UDPReceiver::start(uint16_t udp_port) {
    if (is_running_.load()) {
        return false; // Already running
    }
    
    // Create UDP socket
    udp_socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_fd_ < 0) {
        std::cerr << "[UDP Receiver] Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(udp_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "[UDP Receiver] setsockopt failed: " << strerror(errno) << std::endl;
        close(udp_socket_fd_);
        udp_socket_fd_ = -1;
        return false;
    }
    
    // Bind to port
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(udp_port);
    
    if (bind(udp_socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "[UDP Receiver] Bind failed: " << strerror(errno) << std::endl;
        close(udp_socket_fd_);
        udp_socket_fd_ = -1;
        return false;
    }
    
    udp_port_ = udp_port;
    connection_->set_udp_socket(udp_socket_fd_);
    
    // Start receiver thread
    should_stop_.store(false);
    receiver_thread_ = std::thread(&UDPReceiver::receiver_thread_func, this);
    
    // Wait a bit for thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    std::cout << "[UDP Receiver] Started on port " << udp_port << std::endl;
    return true;
}

inline void UDPReceiver::stop() {
    if (is_running_.load()) {
        should_stop_.store(true);
        
        // Wait for thread to finish
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
        
        // Close socket
        if (udp_socket_fd_ >= 0) {
            close(udp_socket_fd_);
            udp_socket_fd_ = -1;
        }
        
        is_running_.store(false);
    }
}

inline void UDPReceiver::receiver_thread_func() {
    is_running_.store(true);
    
    const size_t max_packet_size = sizeof(SDRPacketHeader) + SDRPacket::MAX_PAYLOAD_SIZE;
    uint8_t recv_buffer[max_packet_size];
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    std::cout << "[UDP Receiver] Thread started, waiting for packets..." << std::endl;
    
    while (!should_stop_.load(std::memory_order_acquire)) {
        // Set socket timeout for polling
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout
        setsockopt(udp_socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        ssize_t n = recvfrom(udp_socket_fd_, recv_buffer, max_packet_size, 0,
                            (struct sockaddr*)&client_addr, &client_len);
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // Timeout or interrupted, continue
                continue;
            }
            std::cerr << "[UDP Receiver] Recvfrom failed: " << strerror(errno) << std::endl;
            break;
        }
        
        if (n < static_cast<ssize_t>(sizeof(SDRPacketHeader))) {
            std::cerr << "[UDP Receiver] Packet too small: " << n << " bytes" << std::endl;
            continue;
        }
        
        // Parse header
        SDRPacketHeader header;
        std::memcpy(&header, recv_buffer, sizeof(SDRPacketHeader));
        header.to_host_order();
        
        // Validate header
        if (!header.is_valid()) {
            std::cerr << "[UDP Receiver] Invalid packet header (magic mismatch)" << std::endl;
            continue;
        }
        
        // Get payload
        const uint8_t* payload = recv_buffer + sizeof(SDRPacketHeader);
        size_t actual_payload_len = n - sizeof(SDRPacketHeader);
        size_t expected_payload_len = header.payload_len;
        
        // Use the smaller of actual received length or expected length
        // But ensure we have at least some data
        size_t payload_len = std::min(actual_payload_len, static_cast<size_t>(expected_payload_len));
        
        // Debug: log if there's a mismatch
        if (actual_payload_len != expected_payload_len) {
            std::cout << "[UDP Receiver] Packet " << header.packet_offset 
                      << ": received " << actual_payload_len 
                      << " bytes, expected " << expected_payload_len << std::endl;
        }
        
        // Process packet
        process_packet(header, payload, payload_len);
    }
    
    is_running_.store(false);
    std::cout << "[UDP Receiver] Thread stopped" << std::endl;
}

inline void UDPReceiver::process_packet(const SDRPacketHeader& header, 
                                       const uint8_t* payload, size_t payload_len) {
    // Get message context
    MessageContext* msg_ctx = connection_->get_message(header.msg_id);
    
    if (!msg_ctx) {
        // Message doesn't exist (could be late packet or invalid msg_id)
        return;
    }
    
    // Check generation (basic late packet protection)
    if (msg_ctx->generation != header.transfer_id) {
        // Different generation, ignore
        return;
    }
    
    // Check state
    if (msg_ctx->state == MessageState::COMPLETED || 
        msg_ctx->state == MessageState::NULL_STATE) {
        // Message already completed, ignore late packet
        return;
    }
    
        // Write packet to buffer
        write_packet_to_buffer(msg_ctx, header.packet_offset, payload, payload_len);
        
        // Update backend packet bitmap
        if (msg_ctx->backend_bitmap) {
            msg_ctx->backend_bitmap->set_packet_received(header.packet_offset);
            // Removed verbose logging - progress is shown via chunk bitmap display
        }
}

inline void UDPReceiver::write_packet_to_buffer(MessageContext* msg_ctx, 
                                                uint32_t packet_offset,
                                                const uint8_t* payload, size_t payload_len) {
    if (!msg_ctx->buffer) {
        std::cerr << "[UDP Receiver] Error: msg_ctx->buffer is null!" << std::endl;
        return;
    }
    
    // Calculate buffer offset using MTU from connection params
    uint32_t mtu_bytes = msg_ctx->connection_params.mtu_bytes;
    if (mtu_bytes == 0) {
        std::cerr << "[UDP Receiver] Warning: mtu_bytes is 0, using packet_offset directly" << std::endl;
        mtu_bytes = 1;
    }
    
    size_t buffer_offset = static_cast<size_t>(packet_offset) * static_cast<size_t>(mtu_bytes);
    
    // Removed verbose debug output - progress is shown via chunk bitmap display
    
    // Check bounds
    if (buffer_offset >= msg_ctx->buffer_size) {
        std::cerr << "[UDP Receiver] Error: buffer_offset (" << buffer_offset 
                  << ") >= buffer_size (" << msg_ctx->buffer_size << ")" << std::endl;
        return;
    }
    
    if (buffer_offset + payload_len > msg_ctx->buffer_size) {
        // Adjust payload length if it exceeds buffer
        size_t old_len = payload_len;
        payload_len = msg_ctx->buffer_size - buffer_offset;
        std::cerr << "[UDP Receiver] Warning: Truncating payload from " << old_len 
                  << " to " << payload_len << " bytes" << std::endl;
        if (payload_len == 0) {
            return;
        }
    }
    
    // Write payload to buffer
    std::memcpy(static_cast<uint8_t*>(msg_ctx->buffer) + buffer_offset, payload, payload_len);
    
    // Removed verbose logging - progress is shown via chunk bitmap display
}

} // namespace sdr

