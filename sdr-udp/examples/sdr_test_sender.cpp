#include "sdr_api.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>

using namespace sdr;

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <tcp_port> <udp_port> [message_size]" << std::endl;
        return 1;
    }
    
    const char* server_ip = argv[1];
    uint16_t tcp_port = static_cast<uint16_t>(std::stoi(argv[2]));
    uint16_t udp_port = static_cast<uint16_t>(std::stoi(argv[3]));
    size_t message_size = 1024 * 1024; // 1 MiB default
    
    if (argc >= 5) {
        message_size = std::stoull(argv[4]);
    }
    
    std::cout << "[Sender] Starting SDR sender..." << std::endl;
    std::cout << "[Sender] Server: " << server_ip << ":" << tcp_port << std::endl;
    std::cout << "[Sender] UDP port: " << udp_port << std::endl;
    std::cout << "[Sender] Message size: " << message_size << " bytes" << std::endl;
    
    // Create SDR context
    SDRContext* ctx = sdr_ctx_create("sender");
    if (!ctx) {
        std::cerr << "[Sender] Failed to create SDR context" << std::endl;
        return 1;
    }
    
    // Connect to receiver
    SDRConnection* conn = sdr_connect(ctx, server_ip, tcp_port);
    if (!conn) {
        std::cerr << "[Sender] Failed to connect" << std::endl;
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::cout << "[Sender] Connected!" << std::endl;
    
    // Prepare send buffer
    std::vector<uint8_t> send_buffer(message_size);
    for (size_t i = 0; i < message_size; ++i) {
        send_buffer[i] = static_cast<uint8_t>(i % 256);
    }
    
    // Send message (one-shot)
    std::cout << "[Sender] Sending message..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();
    
    SDRSendHandle* send_handle = nullptr;
    if (sdr_send_post(conn, send_buffer.data(), message_size, &send_handle) != 0) {
        std::cerr << "[Sender] Failed to send" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    // Poll for completion
    sdr_send_poll(send_handle);
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    std::cout << "[Sender] Sent " << send_handle->packets_sent << " packets in " 
              << duration.count() << " ms" << std::endl;
    
    // Cleanup
    delete send_handle;
    sdr_disconnect(conn);
    sdr_ctx_destroy(ctx);
    
    std::cout << "[Sender] Done!" << std::endl;
    return 0;
}

