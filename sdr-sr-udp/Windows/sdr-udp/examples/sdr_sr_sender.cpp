#include "sdr_api.h"
#include "config_parser.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <iomanip>

using namespace sdr;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <tcp_port> [message_size_kb]" << std::endl;
        std::cerr << "Example: " << argv[0] << " 127.0.0.1 8888 1024" << std::endl;
        return 1;
    }
    
    const char* server_ip = argv[1];
    uint16_t tcp_port = static_cast<uint16_t>(std::stoi(argv[2]));
    size_t message_size_kb = 1024; // Default 1 MB
    
    if (argc >= 4) {
        message_size_kb = std::stoull(argv[3]);
    }
    
    size_t message_size = message_size_kb * 1024;
    
    std::cout << "========================================" << std::endl;
    std::cout << "  SDR Selective Repeat Test - SENDER" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "[Sender] Server: " << server_ip << ":" << tcp_port << std::endl;
    std::cout << "[Sender] Message size: " << message_size_kb << " KB (" 
              << message_size << " bytes)" << std::endl;
    std::cout << std::endl;
    
    // Create context
    SDRContext* ctx = sdr_ctx_create("sender");
    if (!ctx) {
        std::cerr << "[Sender] ERROR: Failed to create SDR context" << std::endl;
        return 1;
    }
    std::cout << "[Sender] ✓ SDR context created" << std::endl;
    
    // Connect to receiver
    std::cout << "[Sender] Connecting to receiver..." << std::endl;
    SDRConnection* conn = sdr_connect(ctx, server_ip, tcp_port);
    if (!conn) {
        std::cerr << "[Sender] ERROR: Failed to connect to receiver" << std::endl;
        sdr_ctx_destroy(ctx);
        return 1;
    }
    std::cout << "[Sender] ✓ Connected to receiver!" << std::endl;
    std::cout << std::endl;
    
    // Prepare test data with pattern
    std::cout << "[Sender] Preparing test data..." << std::endl;
    std::vector<uint8_t> send_buffer(message_size);
    
    // Fill with recognizable pattern: incrementing bytes
    for (size_t i = 0; i < message_size; ++i) {
        send_buffer[i] = static_cast<uint8_t>(i % 256);
    }
    std::cout << "[Sender] ✓ Test data prepared (pattern: 0-255 repeating)" << std::endl;
    std::cout << std::endl;
    
    // Phase 1: Initial transmission
    std::cout << "========================================" << std::endl;
    std::cout << "  PHASE 1: Initial Transmission" << std::endl;
    std::cout << "========================================" << std::endl;
    
    auto start_time = std::chrono::steady_clock::now();
    
    SDRSendHandle* send_handle = nullptr;
    if (sdr_send_post(conn, send_buffer.data(), message_size, &send_handle) != 0) {
        std::cerr << "[Sender] ERROR: Failed to send initial transmission" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    auto initial_end = std::chrono::steady_clock::now();
    auto initial_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        initial_end - start_time);
    
    std::cout << "[Sender] ✓ Initial transmission complete" << std::endl;
    std::cout << "[Sender]   Packets sent: " << send_handle->packets_sent << std::endl;
    std::cout << "[Sender]   Time: " << initial_duration.count() << " ms" << std::endl;
    
    if (send_handle->packets_sent > 0) {
        double throughput_mbps = (message_size * 8.0 / 1000000.0) / 
                                 (initial_duration.count() / 1000.0);
        std::cout << "[Sender]   Throughput: " << std::fixed << std::setprecision(2) 
                  << throughput_mbps << " Mbps" << std::endl;
    }
    std::cout << std::endl;
    
    // Phase 2: Selective Repeat (if needed)
    std::cout << "========================================" << std::endl;
    std::cout << "  PHASE 2: Selective Repeat Protocol" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "[Sender] Starting SR protocol..." << std::endl;
    std::cout << "[Sender] Window size: " << send_handle->window_size << std::endl;
    std::cout << "[Sender] Waiting for ACKs and handling retransmissions..." << std::endl;
    std::cout << std::endl;
    
    auto sr_start = std::chrono::steady_clock::now();
    
    int sr_result = sdr_send_selective_repeat(send_handle);
    
    auto sr_end = std::chrono::steady_clock::now();
    auto sr_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        sr_end - sr_start);
    
    if (sr_result != 0) {
        std::cerr << "[Sender] ERROR: Selective Repeat failed" << std::endl;
        delete send_handle;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::cout << "[Sender] ✓ Selective Repeat complete" << std::endl;
    std::cout << "[Sender]   SR Time: " << sr_duration.count() << " ms" << std::endl;
    std::cout << std::endl;
    
    // Phase 3: Final ACK already received in SR
    std::cout << "========================================" << std::endl;
    std::cout << "  PHASE 3: Transfer Complete" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "[Sender] ✓ COMPLETE_ACK received (during SR phase)" << std::endl;
    
    auto end_time = std::chrono::steady_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    
    std::cout << std::endl;
    
    // Summary
    std::cout << "========================================" << std::endl;
    std::cout << "  TRANSFER SUMMARY" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "[Sender] Total time: " << total_duration.count() << " ms" << std::endl;
    std::cout << "[Sender] Data transferred: " << message_size_kb << " KB" << std::endl;
    
    if (total_duration.count() > 0) {
        double avg_throughput = (message_size * 8.0 / 1000000.0) / 
                               (total_duration.count() / 1000.0);
        std::cout << "[Sender] Average throughput: " << std::fixed << std::setprecision(2) 
                  << avg_throughput << " Mbps" << std::endl;
    }
    
    std::cout << "[Sender] Status: SUCCESS ✓" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Cleanup
    delete send_handle;
    sdr_disconnect(conn);
    sdr_ctx_destroy(ctx);
    
    return 0;
}