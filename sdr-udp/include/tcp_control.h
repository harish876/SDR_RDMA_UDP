#pragma once

#include <cstdint>
#include <string>

namespace sdr {

// Message types for control plane
enum class ControlMsgType : uint8_t {
    OFFER = 0,          // Sender proposes connection parameters
    CTS = 1,            // Clear to Send (receiver ready)
    ACCEPT = 2,         // Receiver accepts offer parameters
    REJECT = 3,         // Receiver rejects offer
    COMPLETE_ACK = 4,   // Receiver acknowledges transfer completion
    INCOMPLETE_NACK = 5,// Receiver indicates transfer incomplete (timeout/packet loss)
    SR_ACK = 6,         // Selective Repeat ACK (cumulative + bitmap window)
    SR_NACK = 7,        // Selective Repeat NACK (gap hint or timeout)
    EC_ACK = 8,         // Erasure coding ACK (decode success)
    EC_NACK = 9         // Erasure coding NACK (fallback request)
};

// Connection parameters structure (used in OFFER and CTS)
struct ConnectionParams {
    uint32_t transfer_id;           // Unique transfer identifier
    uint64_t total_bytes;           // Total message size in bytes
    uint32_t mtu_bytes;              // Maximum Transmission Unit
    uint32_t packet_bytes;          // Size of each packet payload (usually MTU)
    uint32_t chunk_bytes;            // Size of each chunk (multiple of packet_bytes)
    uint16_t packets_per_chunk;      // Number of packets per chunk (P)
    uint16_t total_chunks;           // Total number of chunks (C)
    uint16_t fec_k;                  // FEC data chunks (for future use)
    uint16_t fec_m;                  // FEC parity chunks (for future use)
    uint32_t max_inflight;           // Maximum in-flight messages
    uint32_t rto_ms;                 // Retransmission timeout in milliseconds
    uint32_t rtt_alpha_ms;           // RTT alpha coefficient (for future SR use)
    uint16_t num_channels;           // Number of UDP channels (Section 3.4)
    uint16_t channel_base_port;      // Base port; channels use base + id
    
    // Network parameters
    char udp_server_ip[16];          // Receiver's UDP server IP
    uint16_t udp_server_port;        // Receiver's UDP server port
};

// Control message structure (sent over TCP)
struct ControlMessage {
    uint16_t magic;                  // Magic number for validation (0xSDR0)
    ControlMsgType msg_type;
    uint32_t connection_id;          // Connection identifier
    ConnectionParams params;         // Connection parameters
    
    // Serialization helpers
    size_t serialize(uint8_t* buffer, size_t buffer_size) const;
    bool deserialize(const uint8_t* buffer, size_t buffer_size);
    
    static constexpr uint16_t MAGIC_VALUE = 0x5344; // "SD" in ASCII
};

// TCP Control Server (Receiver side)
class TCPControlServer {
public:
    TCPControlServer();
    ~TCPControlServer();
    
    bool start_listening(uint16_t port);
    
    bool accept_connection();
    
    bool receive_message(ControlMessage& msg);
    
    bool send_message(const ControlMessage& msg);
    
    void close_connection();
    
    void stop();
    
    uint16_t get_listen_port() const { return listen_port_; }
    int get_client_fd() const { return client_fd_; }
    
private:
    int listen_fd_;
    int client_fd_;
    uint16_t listen_port_;
    bool is_listening_;
};

// TCP Control Client (Sender side)
class TCPControlClient {
public:
    TCPControlClient();
    ~TCPControlClient();
    
    bool connect_to_server(const std::string& server_ip, uint16_t server_port);
    
    bool send_message(const ControlMessage& msg);

    bool receive_message(ControlMessage& msg);
    
    void disconnect();
    
    bool is_connected() const { return is_connected_; }
    
private:
    int socket_fd_;
    bool is_connected_;
};

class ConnectionIDAllocator {
public:
    static uint32_t allocate() {
        static uint32_t next_id = 1;
        return next_id++;
    }
    
    static void reset() {
        // Note: Not thread-safe, for testing only
        // In production, use atomic counter
    }
};

} // namespace sdr
