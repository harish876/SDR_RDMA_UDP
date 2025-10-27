#include "erasure_code.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <random>

namespace EC {

UDPSender::UDPSender(const std::string& host, int port, const Config& config) 
    : host_(host), port_(port), config_(config), rs_(config.k, config.m), stats_{} {
    if (!create_socket()) {
        throw std::runtime_error("Failed to create UDP socket");
    }
}

UDPSender::~UDPSender() {
    close_socket();
}

bool UDPSender::create_socket() {
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        std::cerr << "Error creating socket: " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

void UDPSender::close_socket() {
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }
}

bool UDPSender::send_data(const std::vector<uint8_t>& data) {
    // Pad data to fit packet size
    size_t padded_size = ((data.size() + config_.packet_size - 1) / config_.packet_size) * config_.packet_size;
    std::vector<uint8_t> padded_data = data;
    padded_data.resize(padded_size, 0);
    
    // Encode data using Reed-Solomon
    auto encoded_packets = rs_.encode(padded_data);
    
    // Create and send packets
    for (size_t i = 0; i < encoded_packets.size(); ++i) {
        PacketType type = (i < static_cast<size_t>(config_.k)) ? PacketType::DATA : PacketType::PARITY;
        Packet packet(static_cast<uint32_t>(i), type, encoded_packets[i]);
        
        if (!send_packet(packet)) {
            std::cerr << "Failed to send packet " << i << std::endl;
            return false;
        }
    }
    
    return true;
}

bool UDPSender::send_packet(const Packet& packet) {
    auto serialized = packet.serialize();
    
    // Simulate packet loss for testing
    if (Utils::should_drop_packet(0.1)) {  // 10% loss rate
        std::cout << "Simulated packet loss for sequence " << packet.header.sequence << std::endl;
        return true;  // Pretend it was sent
    }
    
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port_);
    
    if (inet_pton(AF_INET, host_.c_str(), &dest_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address: " << host_ << std::endl;
        return false;
    }
    
    ssize_t bytes_sent = sendto(sockfd_, serialized.data(), serialized.size(), 0,
                               (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    
    if (bytes_sent < 0) {
        std::cerr << "Error sending packet: " << strerror(errno) << std::endl;
        return false;
    }
    
    stats_.packets_sent++;
    stats_.bytes_sent += bytes_sent;
    
    return true;
}

void UDPSender::handle_control_packet(const Packet& packet) {
    if (packet.header.type != PacketType::CONTROL) {
        return;
    }
    
    // Parse control packet
    if (packet.data.size() < 1) {
        return;
    }
    
    ControlType control_type = static_cast<ControlType>(packet.data[0]);
    
    switch (control_type) {
        case ControlType::ACK:
            stats_.acks_received++;
            std::cout << "Received ACK for sequence " << packet.header.sequence << std::endl;
            break;
            
        case ControlType::NACK:
            stats_.nacks_received++;
            std::cout << "Received NACK for sequence " << packet.header.sequence << std::endl;
            // In a real implementation, we would retransmit the requested packets
            break;
            
        case ControlType::COMPLETE:
            std::cout << "Receiver confirmed data reception complete" << std::endl;
            break;
            
        default:
            break;
    }
}

} // namespace EC
