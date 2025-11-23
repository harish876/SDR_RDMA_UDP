#include "sdr_api.h"
#include "reliability/sr.h"
#include "reliability/ec.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <memory>

using namespace sdr;
using sdr::reliability::SRSender;
using sdr::reliability::SRConfig;
using sdr::reliability::ECSender;
using sdr::reliability::ECConfig;

int main(int argc, char* argv[]) {
    // Optional mode flag: --mode sdr|sr|ec
    enum class Mode { SDR, SR, EC };
    Mode mode = Mode::SDR;
    int argi = 1;
    if (argc > 1 && std::string(argv[1]) == "--mode") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " [--mode sdr|sr|ec] <server_ip> <tcp_port> <udp_port> [message_size]\n";
            return 1;
        }
        std::string m = argv[2];
        if (m == "sr") mode = Mode::SR;
        else if (m == "ec") mode = Mode::EC;
        else mode = Mode::SDR;
        argi = 3;
    }
    if (argc - argi < 3) {
        std::cerr << "Usage: " << argv[0] << " [--mode sdr|sr|ec] <server_ip> <tcp_port> <udp_port> [message_size]" << std::endl;
        return 1;
    }
    
    const char* server_ip = argv[argi + 0];
    uint16_t tcp_port = static_cast<uint16_t>(std::stoi(argv[argi + 1]));
    uint16_t udp_port = static_cast<uint16_t>(std::stoi(argv[argi + 2]));
    size_t message_size = 1024 * 1024; // 1 MiB default
    
    if (argc - argi >= 4) {
        message_size = std::stoull(argv[argi + 3]);
    }
    
    std::cout << "[Sender] Starting SDR sender (mode="
              << (mode == Mode::SDR ? "sdr" : mode == Mode::SR ? "sr" : "ec") << ")..." << std::endl;
    std::cout << "[Sender] Server: " << server_ip << ":" << tcp_port << std::endl;
    std::cout << "[Sender] UDP port: " << udp_port << std::endl;
    std::cout << "[Sender] Message size: " << message_size << " bytes" << std::endl;
    
    SDRContext* ctx = sdr_ctx_create("sender");
    if (!ctx) {
        std::cerr << "[Sender] Failed to create SDR context" << std::endl;
        return 1;
    }
    
    SDRConnection* conn = sdr_connect(ctx, server_ip, tcp_port);
    if (!conn) {
        std::cerr << "[Sender] Failed to connect" << std::endl;
        sdr_ctx_destroy(ctx);
        return 1;
    }
    
    std::cout << "[Sender] Connected!" << std::endl;
    
    std::vector<uint8_t> send_buffer(message_size);
    for (size_t i = 0; i < message_size; ++i) {
        send_buffer[i] = static_cast<uint8_t>(i % 256);
    }
    
    std::cout << "[Sender] Sending message..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();
    
    std::unique_ptr<SDRSendHandle, void(*)(SDRSendHandle*)> send_handle(nullptr, [](SDRSendHandle* h){ delete h; });
    int rc = 0;
    if (mode == Mode::SR) {
        SRConfig sr_cfg{};
        sr_cfg.rto_ms = 0;
        sr_cfg.nack_delay_ms = 0;
        sr_cfg.max_inflight_chunks = 0;
        SRSender sr_sender(sr_cfg);
        rc = sr_sender.start_send(conn, send_buffer.data(), message_size);
        if (rc == 0) {
            rc = sr_sender.poll();
        }
        if (rc != 0) {
            std::cerr << "[Sender][SR] Send failed\n";
        }
    } else if (mode == Mode::EC) {
        ECConfig ec_cfg{};
        ec_cfg.k_data = 0;
        ec_cfg.m_parity = 0;
        ec_cfg.fallback_timeout_ms = 0;
        ECSender ec_sender(ec_cfg);
        rc = ec_sender.encode_and_send(conn, send_buffer.data(), message_size);
        if (rc == 0) {
            rc = ec_sender.poll();
        }
        if (rc != 0) {
            std::cerr << "[Sender][EC] Send failed\n";
        }
    } else {
        SDRSendHandle* raw_handle = nullptr;
        if (sdr_send_post(conn, send_buffer.data(), message_size, &raw_handle) != 0) {
            std::cerr << "[Sender] Failed to send" << std::endl;
            sdr_disconnect(conn);
            sdr_ctx_destroy(ctx);
            return 1;
        }
        send_handle.reset(raw_handle);
        sdr_send_poll(send_handle.get());
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    if (mode == Mode::SDR && send_handle) {
        std::cout << "[Sender] Sent " << send_handle->packets_sent << " packets in " 
                  << duration.count() << " ms" << std::endl;
    } else {
        std::cout << "[Sender] Mode " << (mode == Mode::SR ? "SR" : "EC")
                  << " completed in " << duration.count() << " ms (rc=" << rc << ")\n";
    }
    
    sdr_disconnect(conn);
    sdr_ctx_destroy(ctx);
    
    std::cout << "[Sender] Done!" << std::endl;
    return 0;
}
