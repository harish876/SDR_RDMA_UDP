#include "sdr_api.h"
#include "config_parser.h"
#include "reliability/sr.h"
#include "reliability/ec.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>
#include <optional>

using namespace sdr;
using sdr::reliability::SRReceiver;
using sdr::reliability::SRConfig;
using sdr::reliability::ECReceiver;
using sdr::reliability::ECConfig;

int main(int argc, char* argv[]) {
    // Optional mode flag: --mode sdr|sr|ec
    enum class Mode { SDR, SR, EC };
    Mode mode = Mode::SDR;
    int argi = 1;
    if (argc > 1 && std::string(argv[1]) == "--mode") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " [--mode sdr|sr|ec] <tcp_port> <udp_port> [message_size] <config_file>\n";
            return 1;
        }
        std::string m = argv[2];
        if (m == "sr") mode = Mode::SR;
        else if (m == "ec") mode = Mode::EC;
        else mode = Mode::SDR;
        argi = 3;
    }

    if (argc - argi < 3) {
        std::cerr << "Usage: " << argv[0] << " [--mode sdr|sr|ec] <tcp_port> <udp_port> [message_size] <config_file>" << std::endl;
        std::cerr << "  config_file: required path to .config file" << std::endl;
        return 1;
    }
    
    uint16_t tcp_port = static_cast<uint16_t>(std::stoi(argv[argi + 0]));
    uint16_t udp_port = static_cast<uint16_t>(std::stoi(argv[argi + 1]));
    size_t message_size = 1024 * 1024; // 1 MiB default
    
    std::string config_file;
    if (argc - argi >= 4) {
        message_size = std::stoull(argv[argi + 2]);
        config_file = argv[argi + 3];
    } else {
        config_file = argv[argi + 2];
    }
    
    std::cout << "[Receiver] Starting SDR receiver (mode=" 
              << (mode == Mode::SDR ? "sdr" : mode == Mode::SR ? "sr" : "ec") << ")..." << std::endl;
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
    
    SDRContext* ctx = sdr_ctx_create("receiver");
    if (!ctx) {
        std::cerr << "[Receiver] Failed to create SDR context" << std::endl;
        return 1;
    }
    
    SDRConnection* conn = sdr_listen(ctx, tcp_port);
    if (!conn) {
        std::cerr << "[Receiver] Failed to start listening" << std::endl;
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::cout << "[Receiver] Waiting for sender connection..." << std::endl;
    
    if (!conn->tcp_server->accept_connection()) {
        std::cerr << "[Receiver] Failed to accept connection" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::cout << "[Receiver] Connection accepted!" << std::endl;
    
    ConnectionParams params;
    std::memset(&params, 0, sizeof(params));
    
    params.mtu_bytes = config.get_uint32("mtu_bytes", 128);
    params.packets_per_chunk = static_cast<uint16_t>(config.get_uint32("packets_per_chunk", 64));
    params.udp_server_port = static_cast<uint16_t>(udp_port);
    params.channel_base_port = static_cast<uint16_t>(config.get_uint32("channel_base_port", udp_port));
    params.num_channels = static_cast<uint16_t>(config.get_uint32("num_channels", 1));
    std::strncpy(params.udp_server_ip, "127.0.0.1", sizeof(params.udp_server_ip) - 1);
    params.udp_server_ip[sizeof(params.udp_server_ip) - 1] = '\0';
    params.transfer_id = config.get_uint32("transfer_id", 1);
    
    std::cout << "[Receiver] Applied config: mtu_bytes=" << params.mtu_bytes 
              << ", packets_per_chunk=" << params.packets_per_chunk
              << ", num_channels=" << params.num_channels
              << ", channel_base_port=" << params.channel_base_port << std::endl;
    
    if (sdr_set_params(conn, &params) != 0) {
        std::cerr << "[Receiver] Failed to set connection parameters" << std::endl;
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::vector<uint8_t> recv_buffer(message_size);
    std::unique_ptr<SDRRecvHandle, void(*)(SDRRecvHandle*)> recv_handle(nullptr, [](SDRRecvHandle* h){ delete h; });
    SDRRecvHandle* active_handle = nullptr; // non-owning pointer to whichever handle is active
    std::optional<SRReceiver> sr_receiver;
    std::optional<ECReceiver> ec_receiver;

    std::cout << "[Receiver] Ready to receive transfer..." << std::endl;

    if (mode == Mode::SR) {
        SRConfig sr_cfg{};
        sr_cfg.rto_ms = config.get_uint32("sr_rto_ms", 0);
        sr_cfg.nack_delay_ms = config.get_uint32("sr_nack_delay_ms", 0);
        sr_cfg.max_inflight_chunks = static_cast<uint16_t>(config.get_uint32("sr_max_inflight_chunks", 0));
        sr_receiver.emplace(sr_cfg);
        if (sr_receiver->post_receive(conn, recv_buffer.data(), message_size) != 0) {
            std::cerr << "[Receiver] SR post_receive failed\n";
            sdr_disconnect(conn);
            sdr_ctx_destroy(ctx);
            return 1;
        }
        active_handle = sr_receiver->handle();
    } else if (mode == Mode::EC) {
        ECConfig ec_cfg{};
        ec_cfg.k_data = static_cast<uint16_t>(config.get_uint32("ec_k_data", 4));
        ec_cfg.m_parity = static_cast<uint16_t>(config.get_uint32("ec_m_parity", 2));
        ec_cfg.fallback_timeout_ms = config.get_uint32("ec_fallback_timeout_ms", 0);
        ec_cfg.data_bytes = message_size;
        ec_cfg.max_retries = config.get_uint32("ec_max_retries", 3);
        // compute total length with parity
        uint32_t capped_mtu = std::min<uint32_t>(params.mtu_bytes, SDRPacket::MAX_PAYLOAD_SIZE);
        uint32_t chunk_bytes = capped_mtu * params.packets_per_chunk;
        if (chunk_bytes == 0) chunk_bytes = 1;
        uint32_t data_chunks = static_cast<uint32_t>((message_size + chunk_bytes - 1) / chunk_bytes);
        uint32_t stripes = (data_chunks + ec_cfg.k_data - 1) / ec_cfg.k_data;
        uint32_t parity_chunks = stripes * ec_cfg.m_parity;
        size_t total_length = static_cast<size_t>(data_chunks + parity_chunks) * chunk_bytes;
        recv_buffer.resize(total_length);
        ec_receiver.emplace(ec_cfg);
        if (ec_receiver->post_receive(conn, recv_buffer.data(), recv_buffer.size()) != 0) {
            std::cerr << "[Receiver] EC post_receive failed\n";
            sdr_disconnect(conn);
            sdr_ctx_destroy(ctx);
            return 1;
        }
        // override total_chunks for progress display
        if (ec_receiver->handle() && ec_receiver->handle()->msg_ctx) {
            ec_receiver->handle()->msg_ctx->total_chunks = static_cast<uint32_t>(data_chunks + parity_chunks);
        }
        active_handle = ec_receiver->handle();
    } else {
        SDRRecvHandle* raw = nullptr;
        if (sdr_recv_post(conn, recv_buffer.data(), recv_buffer.size(), &raw) != 0) {
            std::cerr << "[Receiver] Failed to post receive" << std::endl;
            sdr_disconnect(conn);
            sdr_ctx_destroy(ctx);
            return 1;
        }
        recv_handle.reset(raw);
        active_handle = recv_handle.get();
    }
    
    std::cout << "[Receiver] Receive posted, waiting for data..." << std::endl;
    

    auto start_time = std::chrono::steady_clock::now();
    size_t chunks_received = 0;
    size_t total_chunks = 0;
    size_t iterations = 0;
    const size_t MAX_ITERATIONS = 1000000;
    // Allow longer for EC mode to accommodate retransmissions/decode cycles
    const std::chrono::seconds TIMEOUT_SECONDS(mode == Mode::EC ? 120 : 30);
    
    const uint8_t* chunk_bitmap = nullptr;
    size_t bitmap_len = 0;
    
    auto load_handle_ctx = [&]() -> MessageContext* {
        if (active_handle && active_handle->msg_ctx) {
            return active_handle->msg_ctx.get();
        }
        return conn->connection_ctx->get_message(0);
    };

    if (auto ctx = load_handle_ctx()) {
        total_chunks = ctx->total_chunks;
        std::cout << "[Receiver] Waiting for " << total_chunks << " chunks..." << std::endl;
    }
    
    if (total_chunks == 0) {
        std::cerr << "[Receiver] Error: total_chunks is 0!" << std::endl;
        if (recv_handle) sdr_recv_complete(recv_handle.get());
        sdr_disconnect(conn);
        sdr_ctx_destroy(ctx);
        return 1;
    }
    

    size_t prev_display_lines = 0;
    const size_t window_size = static_cast<size_t>(config.get_uint32("window_size", 15));
    
    auto display_progress = [&]() {
        MessageContext* ctx = load_handle_ctx();
        if (!ctx || total_chunks == 0 || !ctx->backend_bitmap) {
            return;
        }
        
        for (size_t i = 0; i < prev_display_lines; ++i) {
            std::cout << "\033[A\033[K"; 
        }
        std::cout << "\r\033[K"; 
        
        double percentage = (static_cast<double>(chunks_received) / static_cast<double>(total_chunks)) * 100.0;
        const size_t bar_width = 50;
        size_t filled = static_cast<size_t>((percentage / 100.0) * bar_width);
        if (filled > bar_width) filled = bar_width;
        
        std::cout << "[Receiver] Message Progress: [";
        for (size_t i = 0; i < bar_width; ++i) {
            if (i < filled) {
                std::cout << "=";
            } else {
                std::cout << "-";
            }
        }
        std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "% "
                  << "(" << chunks_received << "/" << total_chunks << " chunks)\n";
        
        uint16_t packets_per_chunk = ctx->packets_per_chunk;
        if (packets_per_chunk == 0) {
            prev_display_lines = 1;
            std::cout << std::flush;
            return;
        }
        
        static size_t window_index = 0;
        size_t num_windows = (total_chunks + window_size - 1) / window_size;
        size_t start_chunk = (num_windows > 0) ? (window_index % num_windows) * window_size : 0;
        size_t end_chunk = std::min(start_chunk + window_size, total_chunks);
        
        if (total_chunks > window_size) {
            std::cout << "Showing chunks " << start_chunk << "-" << (end_chunk - 1)
                      << " (of " << total_chunks << "):\n";
        } else {
            std::cout << "Chunk Progress:\n";
        }
        
        const size_t chunk_bar_width = 30;
        
        for (size_t chunk_id = start_chunk; chunk_id < end_chunk; ++chunk_id) {
            uint32_t packets_received = ctx->backend_bitmap ? ctx->backend_bitmap->get_chunk_packet_count(chunk_id) : 0;
            bool chunk_complete = ctx->backend_bitmap ? ctx->backend_bitmap->is_chunk_complete(chunk_id) : false;
            if (chunk_complete) {
                packets_received = packets_per_chunk; // avoid partial count when complete
            }
            
            double chunk_pct = (static_cast<double>(packets_received) / static_cast<double>(packets_per_chunk)) * 100.0;
            size_t chunk_filled = static_cast<size_t>((chunk_pct / 100.0) * chunk_bar_width);
            if (chunk_filled > chunk_bar_width) chunk_filled = chunk_bar_width;
            
            char fill_char = chunk_complete ? '#' : '=';
            char empty_char = '-';
            
            std::cout << "  Chunk " << std::setw(4) << chunk_id << ": [";
            for (size_t i = 0; i < chunk_bar_width; ++i) {
                if (i < chunk_filled) {
                    std::cout << fill_char;
                } else {
                    std::cout << empty_char;
                }
            }
            std::cout << "] " << std::setw(5) << std::fixed << std::setprecision(1) << chunk_pct << "% "
                      << "(" << std::setw(3) << packets_received << "/" << packets_per_chunk << " packets)";
            
            if (chunk_complete) {
                std::cout << " ✓";
            }
            std::cout << "\n";
        }
        
        prev_display_lines = 1 + 1 + (end_chunk - start_chunk); 
        
        window_index++;
        std::cout << std::flush;
    };
    
    size_t last_chunks_received = 0;
    auto last_progress_time = std::chrono::steady_clock::now();
    bool transfer_incomplete = false;
    size_t display_update_counter = 0;
    
    bool ec_decoded_success = false;

    while (iterations < MAX_ITERATIONS) {
        iterations++;
        display_update_counter++;
        
        if (active_handle) {
            if (sdr_recv_bitmap_get(active_handle, &chunk_bitmap, &bitmap_len) != 0) {
                std::cerr << "[Receiver] Failed to get bitmap" << std::endl;
                break;
            }
        }
        
        if (auto ctx = load_handle_ctx()) {
            if (ctx->frontend_bitmap) {
                chunks_received = ctx->frontend_bitmap->get_total_chunks_completed();
                total_chunks = ctx->total_chunks;
            }
            
            bool chunks_changed = (chunks_received > last_chunks_received);
            if (chunks_changed) {
                last_chunks_received = chunks_received;
                last_progress_time = std::chrono::steady_clock::now();
                display_progress();
            } else if (display_update_counter >= 50) {
                display_progress();
                display_update_counter = 0;
            }
            
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
        
        if (sr_receiver.has_value()) {
            bool done = sr_receiver->pump();
            if (done && total_chunks > 0 && chunks_received >= total_chunks) {
                display_progress();
                std::cout << "\n[Receiver] Transfer completed!" << std::endl;
                break;
            }
        } else if (ec_receiver.has_value()) {
            // Attempt EC decode when enough chunks received
            if (ec_receiver->try_decode()) {
                chunks_received = total_chunks;
                display_progress();
                std::cout << "\n[Receiver][EC] Decode successful, completing transfer" << std::endl;
                ec_decoded_success = true;
                break;
            }
        } else {
            if (total_chunks > 0 && chunks_received >= total_chunks) {
                display_progress();
                std::cout << "\n[Receiver] Transfer completed!" << std::endl;
                break;
            }
        }
    
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
        
        double duration_ms = duration.count();
        double duration_sec = duration_ms / 1000.0;
        if (duration_sec > 0) {
            long long bytes = message_size;
            double mbits_per_sec = (bytes * 8.0) / duration_sec / 1'000'000.0;
            double mb_per_sec = (bytes / (1024.0 * 1024.0)) / duration_sec;
            std::cout << "[Receiver] Throughput: " << mb_per_sec << " MB/s (" 
                      << mbits_per_sec << " Mbit/sec)" << std::endl;
        }
    }
    
    if (transfer_incomplete) {
        std::cerr << "[Receiver] Transfer was incomplete. Missing " << (total_chunks - chunks_received) 
                  << " chunks. This may indicate packet loss or sender disconnect." << std::endl;
    }
    
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
    
    if (active_handle) {
        if (mode == Mode::EC && ec_receiver.has_value() && ec_decoded_success) {
            if (active_handle->msg_ctx && active_handle->msg_ctx->frontend_bitmap) {
                active_handle->msg_ctx->frontend_bitmap->stop_polling();
            }
            if (active_handle->msg_ctx) {
                active_handle->msg_ctx->state = MessageState::COMPLETED;
            }
        } else {
            sdr_recv_complete(active_handle);
        }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    sdr_disconnect(conn);
    sdr_ctx_destroy(ctx);
    
    std::cout << "[Receiver] Done!" << std::endl;
    return 0;
}
