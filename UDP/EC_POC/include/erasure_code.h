#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <chrono>

namespace EC {

// Erasure coding configuration
struct Config {
    int k = 8;                    // Number of data packets
    int m = 2;                    // Number of parity packets
    int packet_size = 1024;       // Size of each packet (bytes)
    int timeout_ms = 1000;        // Retransmission timeout
    int max_retries = 3;          // Maximum retry attempts
    bool enable_nack = true;      // Enable NACK-based retransmission
    
    int total_packets() const { return k + m; }
    int data_size() const { return k * packet_size; }
};

// Packet types
enum class PacketType : uint8_t {
    DATA = 0x01,
    PARITY = 0x02,
    CONTROL = 0x03
};

// Control packet subtypes
enum class ControlType : uint8_t {
    ACK = 0x01,
    NACK = 0x02,
    COMPLETE = 0x03
};

// Packet header structure
struct PacketHeader {
    uint16_t magic;           // Magic number: 0xEC01
    uint8_t version;          // Protocol version
    uint8_t flags;            // Reserved flags
    uint32_t sequence;        // Packet sequence number
    PacketType type;          // Packet type
    uint32_t checksum;        // Simple checksum
    
    static constexpr uint16_t MAGIC_NUMBER = 0xEC01;
    static constexpr uint8_t PROTOCOL_VERSION = 1;
};

// Complete packet structure
struct Packet {
    PacketHeader header;
    std::vector<uint8_t> data;
    
    Packet() = default;
    Packet(uint32_t seq, PacketType type, const std::vector<uint8_t>& payload);
    
    // Serialize packet to bytes
    std::vector<uint8_t> serialize() const;
    
    // Deserialize packet from bytes
    static std::unique_ptr<Packet> deserialize(const uint8_t* data, size_t size);
    
    // Calculate simple checksum
    uint32_t calculate_checksum() const;
    
    // Validate packet
    bool is_valid() const;
};

// Reed-Solomon encoder/decoder
class ReedSolomon {
public:
    ReedSolomon(int k, int m);
    ~ReedSolomon();
    
    // Encode data into k data packets + m parity packets
    std::vector<std::vector<uint8_t>> encode(const std::vector<uint8_t>& data) const;
    
    // Decode data from received packets (with bitmap indicating which packets arrived)
    std::vector<uint8_t> decode(const std::vector<std::vector<uint8_t>>& packets, 
                               const std::vector<bool>& received) const;
    
    int get_k() const { return k_; }
    int get_m() const { return m_; }
    int get_total() const { return k_ + m_; }

private:
    int k_, m_;
    // Reed-Solomon implementation will be added
    void* rs_encoder_;  // Placeholder for actual RS implementation
};

// UDP Sender with erasure coding
class UDPSender {
public:
    UDPSender(const std::string& host, int port, const Config& config = Config{});
    ~UDPSender();
    
    // Send data with erasure coding
    bool send_data(const std::vector<uint8_t>& data);
    
    // Send individual packet
    bool send_packet(const Packet& packet);
    
    // Handle incoming control packets (ACKs/NACKs)
    void handle_control_packet(const Packet& packet);
    
    // Get statistics
    struct Stats {
        uint64_t packets_sent = 0;
        uint64_t bytes_sent = 0;
        uint64_t retransmissions = 0;
        uint64_t acks_received = 0;
        uint64_t nacks_received = 0;
    };
    
    const Stats& get_stats() const { return stats_; }

private:
    std::string host_;
    int port_;
    Config config_;
    int sockfd_;
    ReedSolomon rs_;
    Stats stats_;
    
    bool create_socket();
    void close_socket();
};

// UDP Receiver with erasure coding
class UDPReceiver {
public:
    UDPReceiver(int port, const Config& config = Config{});
    ~UDPReceiver();
    
    // Receive data with erasure coding
    std::vector<uint8_t> receive_data();
    
    // Receive single packet
    std::unique_ptr<Packet> receive_packet();
    
    // Send control packet (ACK/NACK)
    bool send_control_packet(const std::string& host, int port, 
                           ControlType type, const std::vector<uint32_t>& sequences);
    
    // Get statistics
    struct Stats {
        uint64_t packets_received = 0;
        uint64_t bytes_received = 0;
        uint64_t packets_decoded = 0;
        uint64_t packets_lost = 0;
        uint64_t acks_sent = 0;
        uint64_t nacks_sent = 0;
    };
    
    const Stats& get_stats() const { return stats_; }

private:
    int port_;
    Config config_;
    int sockfd_;
    ReedSolomon rs_;
    Stats stats_;
    
    // Packet tracking
    std::vector<bool> received_bitmap_;
    std::vector<std::vector<uint8_t>> packet_buffer_;
    uint32_t current_sequence_;
    bool decoding_in_progress_;
    
    bool create_socket();
    void close_socket();
    void reset_tracking();
    std::vector<uint8_t> try_decode();
    void send_nack_for_missing();
};

// Utility functions
namespace Utils {
    // Simulate packet loss for testing
    bool should_drop_packet(double loss_rate = 0.1);
    
    // Generate random data for testing
    std::vector<uint8_t> generate_test_data(size_t size);
    
    // Calculate simple hash for data integrity
    uint32_t simple_hash(const uint8_t* data, size_t size);
}

} // namespace EC
