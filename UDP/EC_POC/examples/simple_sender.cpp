#include "erasure_code.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace EC;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <host> <port>" << std::endl;
        return 1;
    }
    
    std::string host = argv[1];
    int port = std::stoi(argv[2]);
    
    try {
        // Configure erasure coding
        Config config;
        config.k = 8;           // 8 data packets
        config.m = 2;           // 2 parity packets
        config.packet_size = 1024;
        config.enable_nack = true;
        
        std::cout << "Erasure Coding Config: k=" << config.k 
                  << ", m=" << config.m 
                  << ", packet_size=" << config.packet_size << std::endl;
        
        // Create sender
        UDPSender sender(host, port, config);
        
        // Generate test data
        std::string message = "Hello, Erasure Coding over UDP! This is a test message.";
        std::vector<uint8_t> data(message.begin(), message.end());
        
        std::cout << "Sending data (" << data.size() << " bytes): " << message << std::endl;
        
        // Send data
        if (sender.send_data(data)) {
            std::cout << "Data sent successfully!" << std::endl;
            
            // Print statistics
            const auto& stats = sender.get_stats();
            std::cout << "Statistics:" << std::endl;
            std::cout << "  Packets sent: " << stats.packets_sent << std::endl;
            std::cout << "  Bytes sent: " << stats.bytes_sent << std::endl;
            std::cout << "  Retransmissions: " << stats.retransmissions << std::endl;
            std::cout << "  ACKs received: " << stats.acks_received << std::endl;
            std::cout << "  NACKs received: " << stats.nacks_received << std::endl;
        } else {
            std::cerr << "Failed to send data!" << std::endl;
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
