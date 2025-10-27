#include "erasure_code.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <algorithm>

namespace EC {

UDPReceiver::UDPReceiver(int port, const Config& config) 
    : port_(port), config_(config), rs_(config.k, config.m), stats_{} {
    if (!create_socket()) {
        throw std::runtime_error("Failed to create UDP socket");
    }
    reset_tracking();
}

UDPReceiver::~UDPReceiver() {
    close_socket();
}

bool UDPReceiver::create_socket() {
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        std::cerr << "Error creating socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Error setting socket options: " << strerror(errno) << std::endl;
        close(sockfd_);
        return false;
    }
    
    // Bind to port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    
    if (bind(sockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Error binding socket: " << strerror(errno) << std::endl;
        close(sockfd_);
        return false;
    }
    
    return true;
}

void UDPReceiver::close_socket() {
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }
}

void UDPReceiver::reset_tracking() {
    received_bitmap_.clear();
    packet_buffer_.clear();
    received_bitmap_.resize(config_.total_packets(), false);
    packet_buffer_.resize(config_.total_packets());
    current_sequence_ = 0;
    decoding_in_progress_ = false;
}

std::vector<uint8_t> UDPReceiver::receive_data() {
    reset_tracking();
    
    std::cout << "Waiting for " << config_.total_packets() << " packets..." << std::endl;
    
    // Receive packets until we have enough to decode
    while (true) {
        auto packet = receive_packet();
        if (!packet) {
            continue;
        }
        
        // Check if this is a new sequence
        if (packet->header.sequence != current_sequence_) {
            std::cout << "New sequence detected: " << packet->header.sequence << std::endl;
            reset_tracking();
            current_sequence_ = packet->header.sequence;
        }
        
        // Determine packet index based on type and sequence
        int packet_index = -1;
        if (packet->header.type == PacketType::DATA) {
            packet_index = packet->header.sequence % config_.k;
        } else if (packet->header.type == PacketType::PARITY) {
            packet_index = config_.k + (packet->header.sequence % config_.m);
        }
        
        if (packet_index >= 0 && packet_index < config_.total_packets()) {
            received_bitmap_[packet_index] = true;
            packet_buffer_[packet_index] = packet->data;
            stats_.packets_received++;
            stats_.bytes_received += packet->data.size();
            
            std::cout << "Received packet " << packet_index << " (type: " 
                      << (packet->header.type == PacketType::DATA ? "DATA" : "PARITY") << ")" << std::endl;
            
            // Check if we have enough packets to decode
            int received_count = std::count(received_bitmap_.begin(), received_bitmap_.end(), true);
            if (received_count >= config_.k) {
                std::cout << "Have " << received_count << " packets, attempting to decode..." << std::endl;
                auto decoded_data = try_decode();
                if (!decoded_data.empty()) {
                    stats_.packets_decoded++;
                    std::cout << "Successfully decoded data (" << decoded_data.size() << " bytes)" << std::endl;
                    return decoded_data;
                }
            }
        }
    }
}

std::unique_ptr<Packet> UDPReceiver::receive_packet() {
    uint8_t buffer[65536];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    ssize_t bytes_received = recvfrom(sockfd_, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)&client_addr, &client_len);
    
    if (bytes_received < 0) {
        std::cerr << "Error receiving packet: " << strerror(errno) << std::endl;
        return nullptr;
    }
    
    return Packet::deserialize(buffer, bytes_received);
}

bool UDPReceiver::send_control_packet(const std::string& host, int port, 
                                     ControlType type, const std::vector<uint32_t>& sequences) {
    // Create control packet
    std::vector<uint8_t> control_data;
    control_data.push_back(static_cast<uint8_t>(type));
    
    // Add sequence numbers
    for (uint32_t seq : sequences) {
        control_data.push_back((seq >> 24) & 0xFF);
        control_data.push_back((seq >> 16) & 0xFF);
        control_data.push_back((seq >> 8) & 0xFF);
        control_data.push_back(seq & 0xFF);
    }
    
    Packet control_packet(0, PacketType::CONTROL, control_data);
    auto serialized = control_packet.serialize();
    
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &dest_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address: " << host << std::endl;
        return false;
    }
    
    ssize_t bytes_sent = sendto(sockfd_, serialized.data(), serialized.size(), 0,
                               (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    
    if (bytes_sent < 0) {
        std::cerr << "Error sending control packet: " << strerror(errno) << std::endl;
        return false;
    }
    
    if (type == ControlType::ACK) {
        stats_.acks_sent++;
    } else if (type == ControlType::NACK) {
        stats_.nacks_sent++;
    }
    
    return true;
}

std::vector<uint8_t> UDPReceiver::try_decode() {
    // Count received packets
    int received_count = std::count(received_bitmap_.begin(), received_bitmap_.end(), true);
    
    if (received_count < config_.k) {
        return {};
    }
    
    // Try to decode
    auto decoded_data = rs_.decode(packet_buffer_, received_bitmap_);
    
    if (decoded_data.empty()) {
        std::cout << "Decoding failed, need more packets" << std::endl;
        return {};
    }
    
    return decoded_data;
}

void UDPReceiver::send_nack_for_missing() {
    std::vector<uint32_t> missing_sequences;
    
    for (size_t i = 0; i < received_bitmap_.size(); ++i) {
        if (!received_bitmap_[i]) {
            missing_sequences.push_back(static_cast<uint32_t>(i));
        }
    }
    
    if (!missing_sequences.empty()) {
        // In a real implementation, we would need the sender's address
        // For now, just print what we would send
        std::cout << "Would send NACK for " << missing_sequences.size() << " missing packets" << std::endl;
    }
}

} // namespace EC
