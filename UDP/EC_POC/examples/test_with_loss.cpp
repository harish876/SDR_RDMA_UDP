#include "erasure_code.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

using namespace EC;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <mode> [loss_rate]" << std::endl;
        std::cerr << "  mode: sender|receiver" << std::endl;
        std::cerr << "  loss_rate: packet loss rate (0.0-1.0, default: 0.2)" << std::endl;
        return 1;
    }
    
    std::string mode = argv[1];
    double loss_rate = (argc > 2) ? std::stod(argv[2]) : 0.2;
    
    if (mode == "sender") {
        // Sender mode
        std::string host = "127.0.0.1";
        int port = 4950;
        
        try {
            Config config;
            config.k = 6;           // 6 data packets
            config.m = 3;           // 3 parity packets (can lose up to 3)
            config.packet_size = 512;
            config.enable_nack = true;
            
            std::cout << "Erasure Coding Config: k=" << config.k 
                      << ", m=" << config.m 
                      << ", packet_size=" << config.packet_size << std::endl;
            std::cout << "Simulated loss rate: " << (loss_rate * 100) << "%" << std::endl;
            
            UDPSender sender(host, port, config);
            
            // Generate larger test data
            std::string message = "This is a comprehensive test of erasure coding over UDP. "
                                "We are testing packet loss recovery capabilities. "
                                "The system should be able to recover from up to " + 
                                std::to_string(config.m) + " lost packets out of " + 
                                std::to_string(config.k + config.m) + " total packets.";
            
            std::vector<uint8_t> data(message.begin(), message.end());
            
            std::cout << "Sending data (" << data.size() << " bytes)..." << std::endl;
            std::cout << "Message: " << message << std::endl;
            
            if (sender.send_data(data)) {
                std::cout << "Data sent successfully!" << std::endl;
                
                const auto& stats = sender.get_stats();
                std::cout << "Statistics:" << std::endl;
                std::cout << "  Packets sent: " << stats.packets_sent << std::endl;
                std::cout << "  Bytes sent: " << stats.bytes_sent << std::endl;
                std::cout << "  Retransmissions: " << stats.retransmissions << std::endl;
            } else {
                std::cerr << "Failed to send data!" << std::endl;
                return 1;
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
        
    } else if (mode == "receiver") {
        // Receiver mode
        int port = 4950;
        
        try {
            Config config;
            config.k = 6;           // Must match sender
            config.m = 3;
            config.packet_size = 512;
            config.enable_nack = true;
            
            std::cout << "Erasure Coding Config: k=" << config.k 
                      << ", m=" << config.m 
                      << ", packet_size=" << config.packet_size << std::endl;
            
            UDPReceiver receiver(port, config);
            
            std::cout << "Listening on port " << port << "..." << std::endl;
            std::cout << "Can recover from up to " << config.m << " lost packets" << std::endl;
            
            auto received_data = receiver.receive_data();
            
            if (!received_data.empty()) {
                std::string message(received_data.begin(), received_data.end());
                std::cout << "Successfully received data (" << received_data.size() << " bytes)!" << std::endl;
                std::cout << "Message: " << message << std::endl;
                
                const auto& stats = receiver.get_stats();
                std::cout << "Statistics:" << std::endl;
                std::cout << "  Packets received: " << stats.packets_received << std::endl;
                std::cout << "  Bytes received: " << stats.bytes_received << std::endl;
                std::cout << "  Packets decoded: " << stats.packets_decoded << std::endl;
                std::cout << "  Packets lost: " << stats.packets_lost << std::endl;
                
                // Calculate recovery rate
                double recovery_rate = (double)stats.packets_received / (config.k + config.m);
                std::cout << "  Recovery rate: " << (recovery_rate * 100) << "%" << std::endl;
            } else {
                std::cerr << "Failed to receive data!" << std::endl;
                return 1;
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
        
    } else {
        std::cerr << "Invalid mode. Use 'sender' or 'receiver'" << std::endl;
        return 1;
    }
    
    return 0;
}
