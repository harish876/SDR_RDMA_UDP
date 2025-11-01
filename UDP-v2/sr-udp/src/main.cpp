#include "udp_transport.cpp"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // Expect exactly 3 arguments: receiver_ip, receiver_port, total_chunks
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <receiver_ip> <receiver_port> <total_chunks>\n"
                  << "Example: " << argv[0]
                  << " 192.168.65.3 9000 1024\n";
        return 1;
    }

    // Parse arguments
    std::string receiver_ip = argv[1];
    int receiver_port = std::stoi(argv[2]);
    int total_chunks = std::stoi(argv[3]);

    std::cout << "[info] Experiment started.\n";
    std::cout << "[config] Receiver IP: " << receiver_ip
              << ", Port: " << receiver_port
              << ", Total Chunks: " << total_chunks << "\n";

    try {
        UDPTransport transport;
        transport.run_sender(receiver_ip, receiver_port, total_chunks);
    } catch (const std::exception& e) {
        std::cerr << "[error] Exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[info] Experiment finished successfully.\n";
    return 0;
}
