#include "sdr_api.h"
#include "config_parser.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>

using namespace sdr;

void display_progress_bar(size_t current, size_t total, const std::string& label) {
    const size_t bar_width = 50;
    double percentage = total > 0 ? (static_cast<double>(current) / total * 100.0) : 0.0;
    size_t filled = total > 0 ? static_cast<size_t>((percentage / 100.0) * bar_width) : 0;
    
    std::cout << label << " [";
    for (size_t i = 0; i < bar_width; ++i) {
        if (i < filled) std::cout << "=";
        else std::cout << " ";
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "% "
              << "(" << current << "/" << total << ")" << std::endl;
}

bool verify_data(const std::vector<uint8_t>& buffer, size_t size) {
    size_t errors = 0;
    size_t first_error = SIZE_MAX;
    
    for (size_t i = 0; i < size && i < buffer.size(); ++i) {
        uint8_t expected = static_cast<uint8_t>(i % 256);
        if (buffer[i] != expected) {
            if (first_error == SIZE_MAX) {
                first_error = i;
            }
            errors++;
            if (errors > 10) break; // Stop after 10 errors
        }
    }
    
    if (errors > 0) {
        std::cout << "[Receiver] ✗ Data verification FAILED" << std::endl;
        std::cout << "[Receiver]   First error at offset: " << first_error << std::endl;
        std::cout << "[Receiver]   Total errors found: " << errors << std::endl;
        return false;
    } else {
        std::cout << "[Receiver] ✓ Data verification PASSED" << std::endl;
        return true;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <tcp_port> <config_file> [message_size_kb]" << std::endl;
        std::cerr << "Example: " << argv[0] << " 8888 config.ini 1024" << std::endl;
        return 1;
    }
    
    uint16_t tcp_port = static_cast<uint16_t>(std::stoi(argv[1]));
    std::string config_file = argv[2];
    size_t message_size_kb = 1024; // Default 1 MB
    
    if (argc >= 4) {
        message_size_kb = std::stoull(argv[3]);
    }
    
    size_t message_size = message_size_kb * 1024;
    
    std::cout << "========================================" << std::endl;
    std::cout << "  SDR Selective Repeat Test - RECEIVER" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "[Receiver] TCP port: " << tcp_port << std::endl;
    std::cout << "[Receiver] Expected size: " << message_size_kb << " KB" << std::endl;
    std::cout << "[Receiver] Config file: " << config_file << std::endl;
    std::cout << std::endl;
    
    // Load configuration
    std::cout << "[Receiver] Loading configuration..." << std::endl;
    ConfigParser config;
    if (!config.load_from_file(config_file)) {
        std::cerr << "[Receiver] WARNING: Could not load config, using defaults" << std::endl;
    } else {
        config.print_all();
    }
    std::cout << std::endl;
    
    // Create context
    SDRContext* ctx = sdr_ctx_create("receiver");
    if (!ctx) {
        std::cerr << "[Receiver] ERROR: Failed to create SDR context" << std::endl;
        return 1;
    }
    std::cout << "[Receiver] ✓ SDR context created" << std::endl;
    
    // Start listening
    SDRConnection* conn = sdr_listen(ctx, tcp_port);
    if (!conn) {
        std::cerr << "[Receiver] ERROR: Failed to start listening" << std::endl;
        sdr_ctx_destroy(ctx);
        return 1;
    }
    std::cout << "[Receiver] ✓ Listening on port " << tcp_port << std::endl;
    std::cout << std::endl;
    
    // Wait for connection
    std::cout << "========================================" << std::endl;
    std::cout << "  Waiting for sender connection..." << std::endl;
    std::cout << "========================================" << std::endl;
    
    if (!conn->tcp_server->accept_connection()) {
        std::cerr << "[Receiver] ERROR: Failed to accept connection" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    std::cout << "[Receiver] ✓ Sender connected!" << std::endl;
    std::cout << std::endl;
    
    // Configure connection parameters
    ConnectionParams params;
    std::memset(&params, 0, sizeof(params));
    
    params.mtu_bytes = config.get_uint32("mtu_bytes", 1400);
    params.packets_per_chunk = static_cast<uint16_t>(
        config.get_uint32("packets_per_chunk", 32));
    params.udp_server_port = static_cast<uint16_t>(
        config.get_uint32("udp_server_port", 9999));
    params.rto_ms = config.get_uint32("rto_ms", 100);
    params.transfer_id = 1;
    
    std::strncpy(params.udp_server_ip, "127.0.0.1", sizeof(params.udp_server_ip) - 1);
    params.udp_server_ip[sizeof(params.udp_server_ip) - 1] = '\0';
    
    std::cout << "[Receiver] Configuration:" << std::endl;
    std::cout << "  MTU: " << params.mtu_bytes << " bytes" << std::endl;
    std::cout << "  Packets per chunk: " << params.packets_per_chunk << std::endl;
    std::cout << "  UDP port: " << params.udp_server_port << std::endl;
    std::cout << "  RTO: " << params.rto_ms << " ms" << std::endl;
    std::cout << std::endl;
    
    if (sdr_set_params(conn, &params) != 0) {
        std::cerr << "[Receiver] ERROR: Failed to set parameters" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    // Allocate receive buffer
    std::vector<uint8_t> recv_buffer(message_size);
    std::memset(recv_buffer.data(), 0, message_size);
    
    // Post receive
    std::cout << "========================================" << std::endl;
    std::cout << "  RECEIVING DATA" << std::endl;
    std::cout << "========================================" << std::endl;
    
    SDRRecvHandle* recv_handle = nullptr;
    if (sdr_recv_post(conn, recv_buffer.data(), message_size, &recv_handle) != 0) {
        std::cerr << "[Receiver] ERROR: Failed to post receive" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    std::cout << "[Receiver] ✓ Receive posted, waiting for data..." << std::endl;
    
    size_t total_chunks = recv_handle->msg_ctx ? recv_handle->msg_ctx->total_chunks : 0;
    size_t total_packets = recv_handle->msg_ctx ? recv_handle->msg_ctx->total_packets : 0;
    
    std::cout << "[Receiver] Expecting " << total_chunks << " chunks (" 
              << total_packets << " packets)" << std::endl;
    std::cout << std::endl;
    
    // Monitor progress
    auto start_time = std::chrono::steady_clock::now();
    size_t last_chunks = 0;
    auto last_progress_time = start_time;
    const std::chrono::seconds TIMEOUT(30);
    size_t update_counter = 0;
    
    while (true) {
        if (!recv_handle->msg_ctx) break;
        
        size_t chunks_received = recv_handle->msg_ctx->frontend_bitmap->get_total_chunks_completed();
        
        // Update display every 100ms or when progress made
        if (chunks_received != last_chunks || update_counter++ >= 10) {
            update_counter = 0;
            
            // Clear previous lines
            std::cout << "\r\033[K";
            
            display_progress_bar(chunks_received, total_chunks, "Progress");
            
            if (chunks_received != last_chunks) {
                last_chunks = chunks_received;
                last_progress_time = std::chrono::steady_clock::now();
            }
            
            std::cout << "\033[A"; // Move cursor up to overwrite
        }
        
        // Check for completion
        if (chunks_received >= total_chunks) {
            std::cout << "\r\033[K";
            display_progress_bar(chunks_received, total_chunks, "Progress");
            std::cout << "[Receiver] ✓ All chunks received!" << std::endl;
            break;
        }
        
        // Check for timeout
        auto now = std::chrono::steady_clock::now();
        auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_progress_time);
        
        if (idle_time >= TIMEOUT && chunks_received > 0) {
            std::cout << "\r\033[K";
            std::cout << "[Receiver] ✗ Timeout: No progress for " 
                      << TIMEOUT.count() << " seconds" << std::endl;
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    
    std::cout << std::endl;
    
    // Complete receive and send ACK
    std::cout << "========================================" << std::endl;
    std::cout << "  FINALIZING TRANSFER" << std::endl;
    std::cout << "========================================" << std::endl;
    
    size_t final_chunks = recv_handle->msg_ctx->frontend_bitmap->get_total_chunks_completed();
    bool is_complete = (final_chunks >= total_chunks);
    
    std::cout << "[Receiver] Final status: " << final_chunks << "/" << total_chunks 
              << " chunks" << std::endl;
    
    if (sdr_recv_complete(recv_handle) != 0 && is_complete) {
        std::cerr << "[Receiver] WARNING: recv_complete reported incomplete" << std::endl;
    }
    
    std::cout << "[Receiver] ACK sent to sender" << std::endl;
    std::cout << std::endl;
    
    // Verify data
    std::cout << "========================================" << std::endl;
    std::cout << "  DATA VERIFICATION" << std::endl;
    std::cout << "========================================" << std::endl;
    
    bool data_valid = verify_data(recv_buffer, message_size);
    std::cout << std::endl;
    
    // Summary
    std::cout << "========================================" << std::endl;
    std::cout << "  TRANSFER SUMMARY" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "[Receiver] Transfer time: " << duration.count() << " ms" << std::endl;
    std::cout << "[Receiver] Data received: " << message_size_kb << " KB" << std::endl;
    
    if (duration.count() > 0) {
        double throughput = (message_size * 8.0 / 1000000.0) / (duration.count() / 1000.0);
        std::cout << "[Receiver] Throughput: " << std::fixed << std::setprecision(2) 
                  << throughput << " Mbps" << std::endl;
    }
    
    std::cout << "[Receiver] Chunks received: " << final_chunks << "/" << total_chunks << std::endl;
    std::cout << "[Receiver] Data integrity: " << (data_valid ? "PASS ✓" : "FAIL ✗") << std::endl;
    std::cout << "[Receiver] Status: " << (is_complete && data_valid ? "SUCCESS ✓" : "INCOMPLETE ✗") << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    delete recv_handle;
    sdr_disconnect(conn);
    sdr_ctx_destroy(ctx);
    
    return (is_complete && data_valid) ? 0 : 1;
}