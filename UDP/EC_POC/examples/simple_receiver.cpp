#include "erasure_code.h"
#include <iostream>
#include <string>

using namespace EC;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }
    
    int port = std::stoi(argv[1]);
    
    try {
        // Configure erasure coding (must match sender)
        Config config;
        config.k = 8;           // 8 data packets
        config.m = 2;           // 2 parity packets
        config.packet_size = 1024;
        config.enable_nack = true;
        
        std::cout << "Erasure Coding Config: k=" << config.k 
                  << ", m=" << config.m 
                  << ", packet_size=" << config.packet_size << std::endl;
        
        // Create receiver
        UDPReceiver receiver(port, config);
        
        std::cout << "Listening on port " << port << "..." << std::endl;
        
        // Receive data
        auto received_data = receiver.receive_data();
        
        if (!received_data.empty()) {
            // Convert to string for display
            std::string message(received_data.begin(), received_data.end());
            std::cout << "Received data (" << received_data.size() << " bytes): " << message << std::endl;
            
            // Print statistics
            const auto& stats = receiver.get_stats();
            std::cout << "Statistics:" << std::endl;
            std::cout << "  Packets received: " << stats.packets_received << std::endl;
            std::cout << "  Bytes received: " << stats.bytes_received << std::endl;
            std::cout << "  Packets decoded: " << stats.packets_decoded << std::endl;
            std::cout << "  Packets lost: " << stats.packets_lost << std::endl;
            std::cout << "  ACKs sent: " << stats.acks_sent << std::endl;
            std::cout << "  NACKs sent: " << stats.nacks_sent << std::endl;
        } else {
            std::cerr << "Failed to receive data!" << std::endl;
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
