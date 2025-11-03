#include "sdr_api.h"
#include "config_parser.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>

using namespace sdr;

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <tcp_port> <udp_port> [message_size] <config_file>" << std::endl;
        std::cerr << "  config_file: required path to .config file" << std::endl;
        return 1;
    }
    
    uint16_t tcp_port = static_cast<uint16_t>(std::stoi(argv[1]));
    uint16_t udp_port = static_cast<uint16_t>(std::stoi(argv[2]));
    size_t message_size = 1024 * 1024; // 1 MiB default
    
    std::string config_file;
    if (argc >= 5) {
        message_size = std::stoull(argv[3]);
        config_file = argv[4];
    } else {
        config_file = argv[3];
    }
    
    std::cout << "[Receiver] Starting SDR receiver..." << std::endl;
    std::cout << "[Receiver] TCP port: " << tcp_port << std::endl;
    std::cout << "[Receiver] UDP port: " << udp_port << std::endl;
    std::cout << "[Receiver] Expected message size: " << message_size << " bytes" << std::endl;
    
    ConfigParser config;
    bool config_loaded = config.load_from_file(config_file);
    if (!config_loaded) {
        std::cout << "[Receiver] Warning: Could not load config file, using defaults" << std::endl;
    } else {
        config.print_all();
    }
    
    // Create SDR context
    SDRContext* ctx = sdr_ctx_create("receiver");
    if (!ctx) {
        std::cerr << "[Receiver] Failed to create SDR context" << std::endl;
        return 1;
    }
    
    // Listen for connection
    SDRConnection* conn = sdr_listen(ctx, tcp_port);
    if (!conn) {
        std::cerr << "[Receiver] Failed to start listening" << std::endl;
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::cout << "[Receiver] Waiting for sender connection..." << std::endl;
    
    // Accept connection
    if (!conn->tcp_server->accept_connection()) {
        std::cerr << "[Receiver] Failed to accept connection" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::cout << "[Receiver] Connection accepted!" << std::endl;
    
    // Apply configuration to connection parameters
    ConnectionParams params;
    std::memset(&params, 0, sizeof(params));
    
    // Get config values (use defaults if not specified)
    params.mtu_bytes = config.get_uint32("mtu_bytes", 128);
    params.packets_per_chunk = static_cast<uint16_t>(config.get_uint32("packets_per_chunk", 64));
    params.udp_server_port = static_cast<uint16_t>(udp_port); // Use provided UDP port
    std::strncpy(params.udp_server_ip, "127.0.0.1", sizeof(params.udp_server_ip) - 1);
    params.udp_server_ip[sizeof(params.udp_server_ip) - 1] = '\0';
    params.transfer_id = config.get_uint32("transfer_id", 1);
    
    std::cout << "[Receiver] Applied config: mtu_bytes=" << params.mtu_bytes 
              << ", packets_per_chunk=" << params.packets_per_chunk << std::endl;
    
    // Set connection parameters
    if (sdr_set_params(conn, &params) != 0) {
        std::cerr << "[Receiver] Failed to set connection parameters" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    // Allocate receive buffer
    std::vector<uint8_t> recv_buffer(message_size);
    
    // Post receive
    SDRRecvHandle* recv_handle = nullptr;
    if (sdr_recv_post(conn, recv_buffer.data(), message_size, &recv_handle) != 0) {
        std::cerr << "[Receiver] Failed to post receive" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::cout << "[Receiver] Receive posted, waiting for data..." << std::endl;
    
    // Poll chunk bitmap until all chunks are received
    auto start_time = std::chrono::steady_clock::now();
    size_t chunks_received = 0;
    size_t total_chunks = 0;
    size_t iterations = 0;
    const size_t MAX_ITERATIONS = 1000000; // Prevent infinite loop
    
    const uint8_t* chunk_bitmap = nullptr;
    size_t bitmap_len = 0;
    
    // Get initial total_chunks
    if (recv_handle->msg_ctx) {
        total_chunks = recv_handle->msg_ctx->total_chunks;
        std::cout << "[Receiver] Waiting for " << total_chunks << " chunks..." << std::endl;
    }
    
    if (total_chunks == 0) {
        std::cerr << "[Receiver] Error: total_chunks is 0!" << std::endl;
        sdr_recv_complete(recv_handle);
        delete recv_handle;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    // Progress display function - shows chunk-by-chunk packet progress
    auto display_progress = [&]() {
        if (!recv_handle->msg_ctx || !recv_handle->msg_ctx->backend_bitmap) {
            return;
        }
        
        const auto& backend_bitmap = recv_handle->msg_ctx->backend_bitmap;
        uint32_t total_packets = backend_bitmap->get_total_packets();
        uint16_t packets_per_chunk = backend_bitmap->get_packets_per_chunk();
        
        // Clear screen and move to top (simple approach for small number of chunks)
        if (total_chunks <= 20) {
            // Clear previous output
            std::cout << "\033[2J\033[H"; // Clear screen and move cursor to top
            
            // Display header
            std::cout << "[Receiver] Packet Progress (1=received, 0=missing):\n";
            std::cout << "Chunks: " << chunks_received << "/" << total_chunks << "\n\n";
            
            // Display chunk progress
            for (uint32_t chunk_id = 0; chunk_id < total_chunks; ++chunk_id) {
                std::cout << "C" << std::setw(3) << std::setfill('0') << (chunk_id + 1) << " - ";
                
                uint32_t chunk_start_packet = chunk_id * packets_per_chunk;
                uint32_t chunk_end_packet = std::min(chunk_start_packet + packets_per_chunk, total_packets);
                
                for (uint32_t pkt = chunk_start_packet; pkt < chunk_end_packet; ++pkt) {
                    bool received = backend_bitmap->is_packet_received(pkt);
                    std::cout << (received ? "1" : "0") << " ";
                }
                
                // Show completion status
                bool chunk_complete = backend_bitmap->is_chunk_complete(chunk_id);
                std::cout << " [" << (chunk_complete ? "OK" : "--") << "]";
                std::cout << std::endl;
            }
            std::cout << std::flush;
        } else {
            // For many chunks, show summary only
            std::cout << "\r\033[K[Receiver] Chunks: " << chunks_received << "/" << total_chunks << std::flush;
        }
    };
    
    bool progress_shown = false;
    size_t last_display_iteration = 0;
    
    while (iterations < MAX_ITERATIONS) {
        iterations++;
        
        if (sdr_recv_bitmap_get(recv_handle, &chunk_bitmap, &bitmap_len) != 0) {
            std::cerr << "[Receiver] Failed to get bitmap" << std::endl;
            break;
        }
        
        // Count completed chunks
        if (recv_handle->msg_ctx) {
            chunks_received = recv_handle->msg_ctx->frontend_bitmap->get_total_chunks_completed();
            total_chunks = recv_handle->msg_ctx->total_chunks;
            
            // Display progress bar (update every 5 iterations to reduce flicker)
            if (iterations - last_display_iteration >= 5) {
                display_progress();
                progress_shown = true;
                last_display_iteration = iterations;
            }
        }
        
        // Check if all chunks received
        if (total_chunks > 0 && chunks_received >= total_chunks) {
            // Final display
            display_progress();
            std::cout << "\n[Receiver] All chunks received!" << std::endl;
            break;
        }
        
        // Sleep a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    if (progress_shown && total_chunks <= 20) {
        std::cout << std::endl; // Final newline
    }
    
    if (iterations >= MAX_ITERATIONS) {
        std::cerr << "[Receiver] Timeout: Reached maximum iterations" << std::endl;
        std::cerr << "[Receiver] Final status: " << chunks_received << "/" << total_chunks << " chunks" << std::endl;
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    std::cout << "[Receiver] Transfer completed in " << duration.count() << " ms" << std::endl;
    
    // Verify data (simple check)
    bool data_valid = true;
    size_t first_mismatch = SIZE_MAX;
    for (size_t i = 0; i < message_size && i < 1024; ++i) {
        uint8_t expected = static_cast<uint8_t>(i % 256);
        uint8_t actual = recv_buffer[i];
        if (actual != expected) {
            if (first_mismatch == SIZE_MAX) {
                first_mismatch = i;
                std::cerr << "[Receiver] Data mismatch at offset " << i 
                          << ": expected 0x" << std::hex << static_cast<int>(expected)
                          << ", got 0x" << static_cast<int>(actual) << std::dec << std::endl;
            }
            data_valid = false;
            // Don't break immediately, show a few mismatches
            if (i - first_mismatch > 10) break;
        }
    }
    
    if (data_valid) {
        std::cout << "[Receiver] Data verification: PASSED" << std::endl;
    } else {
        std::cout << "[Receiver] Data verification: FAILED (first mismatch at offset " 
                  << first_mismatch << ")" << std::endl;
        // Show first few bytes for debugging
        std::cout << "[Receiver] First 16 bytes received: ";
        for (size_t i = 0; i < std::min(static_cast<size_t>(16), message_size); ++i) {
            std::cout << std::hex << static_cast<int>(recv_buffer[i]) << " ";
        }
        std::cout << std::dec << std::endl;
    }
    
    // Complete receive (stops polling threads)
    sdr_recv_complete(recv_handle);
    
    // Small delay to ensure all threads have finished
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Cleanup
    delete recv_handle;
    sdr_disconnect(conn);
    sdr_ctx_destroy(ctx);
    
    std::cout << "[Receiver] Done!" << std::endl;
    return 0;
}


