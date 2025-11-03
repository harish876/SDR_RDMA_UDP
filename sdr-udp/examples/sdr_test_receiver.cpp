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
    
    std::cout << "[Receiver] Ready to receive transfer..." << std::endl;
    
    // Post receive for single transfer
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
    const size_t MAX_ITERATIONS = 1000000;
    const std::chrono::seconds TIMEOUT_SECONDS(30); // 30 second timeout if no progress
    
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
    
    // Progress display function - shows a nice progress bar
    auto display_progress = [&]() {
        if (!recv_handle->msg_ctx || total_chunks == 0) {
            return;
        }
        
        // Calculate percentage
        double percentage = (static_cast<double>(chunks_received) / static_cast<double>(total_chunks)) * 100.0;
        
        // Progress bar width (in characters)
        const size_t bar_width = 50;
        
        // Calculate how many characters should be filled
        size_t filled = static_cast<size_t>((percentage / 100.0) * bar_width);
        if (filled > bar_width) filled = bar_width;
        
        // Clear the line and move to beginning
        std::cout << "\r\033[K";
        
        // Build progress bar
        std::cout << "[Receiver] [";
        for (size_t i = 0; i < bar_width; ++i) {
            if (i < filled) {
                std::cout << "=";
            } else {
                std::cout << "-";
            }
        }
        
        std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "% "
                  << "(" << chunks_received << "/" << total_chunks << ")" << std::flush;
    };
    
    size_t last_chunks_received = 0;
    auto last_progress_time = std::chrono::steady_clock::now();
    bool transfer_incomplete = false;
    size_t display_update_counter = 0;
    
    // Wait for all chunks to be received
    while (iterations < MAX_ITERATIONS) {
        iterations++;
        display_update_counter++;
        
        if (sdr_recv_bitmap_get(recv_handle, &chunk_bitmap, &bitmap_len) != 0) {
            std::cerr << "[Receiver] Failed to get bitmap" << std::endl;
            break;
        }
        
        // Count completed chunks
        if (recv_handle->msg_ctx) {
            chunks_received = recv_handle->msg_ctx->frontend_bitmap->get_total_chunks_completed();
            total_chunks = recv_handle->msg_ctx->total_chunks;
            
            // Check for progress - if chunks increased, update last progress time and display
            bool chunks_changed = (chunks_received > last_chunks_received);
            if (chunks_changed) {
                last_chunks_received = chunks_received;
                last_progress_time = std::chrono::steady_clock::now();
                // Update display immediately when chunks change
                display_progress();
            } else if (display_update_counter >= 50) {
                // Also update display periodically (every 50 iterations ≈ 500ms) even if no change
                display_progress();
                display_update_counter = 0;
            }
            
            // Check timeout - if no progress for TIMEOUT_SECONDS, consider transfer incomplete
            auto now = std::chrono::steady_clock::now();
            auto time_since_progress = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_progress_time).count();
            
            if (chunks_received > 0 && time_since_progress >= TIMEOUT_SECONDS.count() && 
                chunks_received < total_chunks) {
                std::cout << "\n[Receiver] Timeout: No progress for " << TIMEOUT_SECONDS.count() 
                          << " seconds. Transfer incomplete (" << chunks_received 
                          << "/" << total_chunks << " chunks received)." << std::endl;
                transfer_incomplete = true;
                break;
            }
        }
        
        // Check if all chunks received
        if (total_chunks > 0 && chunks_received >= total_chunks) {
            // Final progress display (100%)
            display_progress();
            std::cout << "\n[Receiver] Transfer completed!" << std::endl;
            break;
        }
        
        // Sleep a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    if (iterations >= MAX_ITERATIONS) {
        std::cerr << "[Receiver] Timeout: Reached maximum iterations" << std::endl;
        std::cerr << "[Receiver] Final status: " << chunks_received << "/" << total_chunks << " chunks" << std::endl;
        transfer_incomplete = true;
    } else if (total_chunks > 0 && chunks_received >= total_chunks) {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "[Receiver] Transfer completed in " << duration.count() << " ms" << std::endl;
    }
    
    if (transfer_incomplete) {
        std::cerr << "[Receiver] Transfer was incomplete. Missing " << (total_chunks - chunks_received) 
                  << " chunks. This may indicate packet loss or sender disconnect." << std::endl;
    }
    
    // Verify data (simple check) - only for completed transfers
    if (total_chunks > 0 && chunks_received >= total_chunks) {
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
                if (i - first_mismatch > 10) break;
            }
        }
        
        if (data_valid) {
            std::cout << "[Receiver] Data verification: PASSED" << std::endl;
        } else {
            std::cout << "[Receiver] Data verification: FAILED (first mismatch at offset " 
                      << first_mismatch << ")" << std::endl;
        }
    }
    
    // Complete receive (stops polling threads)
    sdr_recv_complete(recv_handle);
    
    // Small delay to ensure all threads have finished
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Cleanup
    delete recv_handle;
    
    // Final cleanup
    sdr_disconnect(conn);
    sdr_ctx_destroy(ctx);
    
    std::cout << "[Receiver] Done!" << std::endl;
    return 0;
}


