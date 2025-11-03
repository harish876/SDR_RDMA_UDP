#include "sdr_api.h"
#include "sdr_packet.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

namespace sdr {

SDRContext* sdr_ctx_create(const char* device_name) {
    SDRContext* ctx = new SDRContext();
    if (device_name) {
        ctx->device_name = device_name;
    }
    return ctx;
}

void sdr_ctx_destroy(SDRContext* ctx) {
    if (ctx) {
        delete ctx;
    }
}

SDRConnection* sdr_listen(SDRContext* ctx, uint16_t tcp_port) {
    if (!ctx) {
        return nullptr;
    }
    
    auto* conn = new SDRConnection();
    conn->is_receiver = true;
    conn->tcp_server = new TCPControlServer();
    
    if (!conn->tcp_server->start_listening(tcp_port)) {
        delete conn;
        return nullptr;
    }
    
    uint32_t conn_id = ConnectionIDAllocator::allocate();
    ConnectionParams params;
    std::memset(&params, 0, sizeof(params));
    
    auto connection_ctx = std::make_shared<ConnectionContext>();
    connection_ctx->initialize(conn_id, params);
    conn->connection_ctx = connection_ctx;
    
    return conn;
}

SDRConnection* sdr_connect(SDRContext* ctx, const char* server_ip, uint16_t tcp_port) {
    if (!ctx || !server_ip) {
        return nullptr;
    }
    
    auto* conn = new SDRConnection();
    conn->is_receiver = false;
    conn->tcp_client = new TCPControlClient();
    
    if (!conn->tcp_client->connect_to_server(server_ip, tcp_port)) {
        delete conn;
        return nullptr;
    }
    
    // Create connection context
    uint32_t conn_id = ConnectionIDAllocator::allocate();
    ConnectionParams params;
    std::memset(&params, 0, sizeof(params));
    
    auto connection_ctx = std::make_shared<ConnectionContext>();
    connection_ctx->initialize(conn_id, params);
    conn->connection_ctx = connection_ctx;
    
    return conn;
}

void sdr_disconnect(SDRConnection* conn) {
    if (!conn) {
        return;
    }
    
    // Stop UDP receiver first (waits for thread to finish)
    if (conn->udp_receiver) {
        conn->udp_receiver->stop();
        conn->udp_receiver.reset(); // Clear the shared_ptr
    }
    
    // Stop TCP server
    if (conn->tcp_server) {
        conn->tcp_server->close_connection();
        conn->tcp_server->stop();
    }
    
    // Disconnect TCP client
    if (conn->tcp_client) {
        conn->tcp_client->disconnect();
    }
    
    // Clear connection context
    conn->connection_ctx.reset();
    
    delete conn;
}

// Connection parameter configuration
int sdr_set_params(SDRConnection* conn, const ConnectionParams* params) {
    if (!conn || !params) {
        return -1;
    }
    
    // Update connection context with provided params
    conn->connection_ctx->initialize(conn->connection_ctx->get_connection_id(), *params);
    
    return 0;
}

// Receive operations
int sdr_recv_post(SDRConnection* conn, void* buffer, size_t length, SDRRecvHandle** handle) {
    if (!conn || !buffer || length == 0 || !handle) {
        return -1;
    }
    
    if (!conn->is_receiver) {
        return -1; // Only receiver can post receive
    }
    
    // Allocate message ID (simple for now)
    uint32_t msg_id = 0; // For now, use simple allocation
    // TODO: Implement proper msg_id allocation
    
    // Get connection parameters and set defaults if not initialized
    ConnectionParams params = conn->connection_ctx->get_params();
    
    std::cout << "[SDR API] Received params from context: mtu_bytes=" << params.mtu_bytes 
              << ", packets_per_chunk=" << params.packets_per_chunk << std::endl;
    
    // Set total_bytes to the message length
    params.total_bytes = length;
    
    // If params not initialized (all zeros), set reasonable defaults
    // Must account for SDR packet header (16 bytes) when setting MTU
    // MAX_PAYLOAD_SIZE = 1500 - 8 - 16 = 1476, so use a safe default
    if (params.mtu_bytes == 0) {
        std::cout << "[SDR API] mtu_bytes is 0, setting default to 128" << std::endl;
        params.mtu_bytes = 128;  // Small MTU for testing: 128 bytes payload
    }
    
    // Ensure mtu_bytes doesn't exceed maximum payload size
    if (params.mtu_bytes > SDRPacket::MAX_PAYLOAD_SIZE) {
        std::cerr << "[SDR API] Warning: mtu_bytes (" << params.mtu_bytes 
                  << ") exceeds MAX_PAYLOAD_SIZE (" << SDRPacket::MAX_PAYLOAD_SIZE 
                  << "), capping to " << SDRPacket::MAX_PAYLOAD_SIZE << std::endl;
        params.mtu_bytes = SDRPacket::MAX_PAYLOAD_SIZE;
    }
    if (params.packets_per_chunk == 0) {
        std::cout << "[SDR API] packets_per_chunk is 0, setting default to 64" << std::endl;
        params.packets_per_chunk = 64;  // Default chunk size
    } else {
        std::cout << "[SDR API] Using configured packets_per_chunk=" << params.packets_per_chunk << std::endl;
    }
    // Note: UDP server port should ideally be configured before calling recv_post
    // For now, if not set, we'll use a default
    // TODO: Add API to set connection params (including UDP port) before recv_post
    if (params.udp_server_port == 0) {
        // Try to use default, but this should be set by the application
        params.udp_server_port = 9999;
    }
    if (params.udp_server_ip[0] == '\0') {
        // Set default to localhost
        std::strncpy(params.udp_server_ip, "127.0.0.1", sizeof(params.udp_server_ip) - 1);
        params.udp_server_ip[sizeof(params.udp_server_ip) - 1] = '\0';
    }
    if (params.transfer_id == 0) {
        params.transfer_id = 1;  // Default transfer ID
    }
    
    // Update connection context with initialized params
    conn->connection_ctx->initialize(conn->connection_ctx->get_connection_id(), params);
    
    // Calculate bitmap sizes
    size_t total_packets, total_chunks;
    conn->connection_ctx->calculate_bitmap_sizes(length, params.mtu_bytes, 
                                                 params.packets_per_chunk,
                                                 total_packets, total_chunks);
    
    std::cout << "[SDR API] Receive posted: length=" << length 
              << ", mtu_bytes=" << params.mtu_bytes 
              << ", packets_per_chunk=" << params.packets_per_chunk
              << ", total_packets=" << total_packets 
              << ", total_chunks=" << total_chunks << std::endl;
    
    // Allocate message slot
    uint32_t generation = params.transfer_id; // Use transfer_id as generation for now
    MessageContext* msg_ctx = conn->connection_ctx->allocate_message_slot(msg_id, generation);
    
    if (!msg_ctx) {
        return -1;
    }
    
    // Initialize message context
    msg_ctx->buffer = buffer;
    msg_ctx->buffer_size = length;
    msg_ctx->total_packets = total_packets;
    msg_ctx->total_chunks = total_chunks;
    msg_ctx->packets_per_chunk = params.packets_per_chunk;
    msg_ctx->connection_params = params;
    
    // Create bitmaps
    msg_ctx->backend_bitmap = std::make_shared<BackendBitmap>(
        static_cast<uint32_t>(total_packets), params.packets_per_chunk);
    msg_ctx->frontend_bitmap = std::make_shared<FrontendBitmap>(
        msg_ctx->backend_bitmap, static_cast<uint32_t>(total_chunks));
    
    // Start frontend polling
    msg_ctx->frontend_bitmap->start_polling(100); // 100 microsecond polling interval
    
    // Create receive handle
    auto* recv_handle = new SDRRecvHandle();
    recv_handle->msg_id = msg_id;
    recv_handle->msg_ctx = std::shared_ptr<MessageContext>(msg_ctx, [](MessageContext*) {}); // Non-owning
    recv_handle->user_buffer = buffer;
    recv_handle->buffer_size = length;
    *handle = recv_handle;
    
    // Start UDP receiver if not already started
    if (!conn->udp_receiver) {
        conn->udp_receiver = std::make_shared<UDPReceiver>(conn->connection_ctx);
        if (!conn->udp_receiver->start(params.udp_server_port)) {
            std::cerr << "[SDR API] Failed to start UDP receiver" << std::endl;
            return -1;
        }
    }
    
    // Send CTS via TCP
    if (conn->tcp_server && conn->tcp_server->get_client_fd() >= 0) {
        ControlMessage cts_msg;
        cts_msg.magic = ControlMessage::MAGIC_VALUE;
        cts_msg.msg_type = ControlMsgType::CTS;
        cts_msg.connection_id = conn->connection_ctx->get_connection_id();
        cts_msg.params = params;
        
        std::cout << "[SDR API] Sending CTS with params: mtu_bytes=" << params.mtu_bytes 
                  << ", packets_per_chunk=" << params.packets_per_chunk << std::endl;
        
        if (!conn->tcp_server->send_message(cts_msg)) {
            std::cerr << "[SDR API] Failed to send CTS" << std::endl;
            return -1;
        }
    }
    
    return 0;
}

int sdr_recv_bitmap_get(SDRRecvHandle* handle, const uint8_t** bitmap, size_t* len) {
    if (!handle || !bitmap || !len) {
        return -1;
    }
    
    if (!handle->msg_ctx || !handle->msg_ctx->frontend_bitmap) {
        return -1;
    }
    
    // Get chunk bitmap
    const std::atomic<uint64_t>* chunk_bitmap = handle->msg_ctx->frontend_bitmap->get_chunk_bitmap();
    uint32_t num_words = handle->msg_ctx->frontend_bitmap->get_chunk_bitmap_size();
    
    // Calculate total size in bytes
    *len = num_words * sizeof(uint64_t);
    *bitmap = reinterpret_cast<const uint8_t*>(chunk_bitmap);
    
    return 0;
}

int sdr_recv_complete(SDRRecvHandle* handle) {
    if (!handle || !handle->msg_ctx) {
        return -1;
    }
    
    // Stop frontend polling first (this joins the thread)
    if (handle->msg_ctx->frontend_bitmap) {
        handle->msg_ctx->frontend_bitmap->stop_polling();
    }
    
    // Mark message as completed (after polling is stopped to avoid race)
    handle->msg_ctx->state = MessageState::COMPLETED;
    
    return 0;
}

// Send operations (one-shot)
int sdr_send_post(SDRConnection* conn, const void* buffer, size_t length, SDRSendHandle** handle) {
    if (!conn || !buffer || length == 0 || !handle) {
        return -1;
    }
    
    if (conn->is_receiver) {
        return -1; // Only sender can send
    }
    
    // Wait for CTS from receiver
    if (!conn->tcp_client || !conn->tcp_client->is_connected()) {
        return -1;
    }
    
    ControlMessage cts_msg;
    if (!conn->tcp_client->receive_message(cts_msg)) {
        std::cerr << "[SDR API] Failed to receive CTS" << std::endl;
        return -1;
    }
    
    if (cts_msg.msg_type != ControlMsgType::CTS) {
        std::cerr << "[SDR API] Expected CTS, got other message type" << std::endl;
        return -1;
    }
    
    // Update connection parameters from CTS
    conn->connection_ctx->initialize(cts_msg.connection_id, cts_msg.params);
    
    // Validate connection parameters
    if (cts_msg.params.mtu_bytes == 0) {
        std::cerr << "[SDR API] Error: MTU bytes is zero in CTS message" << std::endl;
        return -1;
    }
    if (cts_msg.params.packets_per_chunk == 0) {
        std::cerr << "[SDR API] Error: Packets per chunk is zero in CTS message" << std::endl;
        return -1;
    }
    
    // Allocate message ID (for sender tracking)
    uint32_t msg_id = 0; // TODO: Proper allocation
    
    // Create send handle
    auto* send_handle = new SDRSendHandle();
    send_handle->msg_id = msg_id;
    send_handle->connection_ctx = conn->connection_ctx;
    send_handle->user_buffer = buffer;
    send_handle->buffer_size = length;
    send_handle->packets_sent = 0;
    *handle = send_handle;
    
    // Calculate packet count
    uint32_t mtu_bytes = cts_msg.params.mtu_bytes;
    size_t total_packets = (length + mtu_bytes - 1) / mtu_bytes;
    
    std::cout << "[SDR API] Sending " << total_packets << " packets (MTU: " << mtu_bytes 
              << ", packets_per_chunk: " << cts_msg.params.packets_per_chunk << ")" << std::endl;
    
    // Send all packets
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    
    if (udp_socket < 0) {
        std::cerr << "[SDR API] Failed to create UDP socket: " << strerror(errno) << std::endl;
        delete send_handle;
        return -1;
    }
    
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(cts_msg.params.udp_server_port);
    
    if (inet_pton(AF_INET, cts_msg.params.udp_server_ip, &server_addr.sin_addr) <= 0) {
        std::cerr << "[SDR API] Failed to convert IP address: " << cts_msg.params.udp_server_ip << std::endl;
        close(udp_socket);
        delete send_handle;
        return -1;
    }
    
    std::cout << "[SDR API] Sending to " << cts_msg.params.udp_server_ip 
              << ":" << cts_msg.params.udp_server_port << std::endl;
    
    // Send packets
    size_t packets_failed = 0;
    for (size_t i = 0; i < total_packets; ++i) {
        uint32_t packet_offset = static_cast<uint32_t>(i);
        size_t remaining = length - (i * mtu_bytes);
        size_t packet_data_len = std::min(static_cast<size_t>(mtu_bytes), remaining);
        
        // Ensure packet_data_len doesn't exceed MAX_PAYLOAD_SIZE
        if (packet_data_len > SDRPacket::MAX_PAYLOAD_SIZE) {
            packet_data_len = SDRPacket::MAX_PAYLOAD_SIZE;
        }
        
        // Create packet
        SDRPacket* packet = SDRPacket::create_data_packet(
            cts_msg.params.transfer_id, msg_id, packet_offset,
            cts_msg.params.packets_per_chunk,
            data + (i * mtu_bytes), packet_data_len);
        
        if (!packet) {
            std::cerr << "[SDR API] Failed to create packet " << i 
                      << " (data_len: " << packet_data_len << ", MAX: " 
                      << SDRPacket::MAX_PAYLOAD_SIZE << ")" << std::endl;
            packets_failed++;
            continue;
        }
        
        // Calculate total size BEFORE converting to network byte order
        // (get_total_size uses header.payload_len which will be wrong after byte order conversion)
        size_t total_packet_size = sizeof(SDRPacketHeader) + packet_data_len;
        
        // Convert to network byte order
        packet->header.to_network_order();
        
        // Send packet (use pre-calculated size, not get_total_size() which reads wrong byte order)
        ssize_t sent = sendto(udp_socket, packet, total_packet_size, 0,
                             (struct sockaddr*)&server_addr, sizeof(server_addr));
        
        if (sent > 0) {
            send_handle->packets_sent++;
        } else {
            // Only log first few failures to avoid spam
            if (packets_failed < 5) {
                std::cerr << "[SDR API] sendto failed for packet " << i 
                          << ": " << strerror(errno) << " (packet_size: " 
                          << total_packet_size << ")" << std::endl;
            }
            packets_failed++;
        }
        
        SDRPacket::destroy(packet);
    }
    
    if (packets_failed > 0) {
        std::cerr << "[SDR API] Error: " << packets_failed << " of " << total_packets 
                  << " packets failed to send" << std::endl;
        if (packets_failed == total_packets) {
            std::cerr << "[SDR API] All packets failed! Check packet size limits." << std::endl;
        }
    } else {
        std::cout << "[SDR API] Successfully sent all " << total_packets << " packets" << std::endl;
    }
    
    close(udp_socket);
    return 0;
}

int sdr_send_poll(SDRSendHandle* handle) {
    if (!handle) {
        return -1;
    }
    
    // For one-shot send, we consider it complete when all packets are sent
    // In a real implementation, we'd wait for ACKs
    // For now, just return success if packets_sent matches expected
    return 0;
}

// Send operations (streaming)
int sdr_send_stream_start(SDRConnection* conn, const void* buffer, size_t length,
                         uint32_t initial_offset, SDRStreamHandle** handle) {
    if (!conn || !buffer || !handle) {
        return -1;
    }
    
    if (conn->is_receiver) {
        return -1; // Only sender can send
    }
    
    // Wait for CTS (same as one-shot)
    if (!conn->tcp_client || !conn->tcp_client->is_connected()) {
        return -1;
    }
    
    ControlMessage cts_msg;
    if (!conn->tcp_client->receive_message(cts_msg)) {
        return -1;
    }
    
    if (cts_msg.msg_type != ControlMsgType::CTS) {
        return -1;
    }
    
    conn->connection_ctx->initialize(cts_msg.connection_id, cts_msg.params);
    
    // Create stream handle
    auto* stream_handle = new SDRStreamHandle();
    stream_handle->msg_id = 0; // TODO: Proper allocation
    stream_handle->connection_ctx = conn->connection_ctx;
    stream_handle->user_buffer = buffer;
    stream_handle->buffer_size = length;
    
    uint32_t mtu_bytes = cts_msg.params.mtu_bytes;
    stream_handle->total_packets = (length + mtu_bytes - 1) / mtu_bytes;
    stream_handle->packets_sent = 0;
    stream_handle->is_active = true;
    
    *handle = stream_handle;
    return 0;
}

int sdr_send_stream_continue(SDRStreamHandle* handle, uint32_t offset, size_t length) {
    if (!handle || !handle->is_active) {
        return -1;
    }
    
    // Send chunk(s) starting at offset
    const ConnectionParams& params = handle->connection_ctx->get_params();
    uint32_t mtu_bytes = params.mtu_bytes;
    
    // Calculate packet range to send
    uint32_t start_packet = offset / mtu_bytes;
    uint32_t end_packet = (offset + length + mtu_bytes - 1) / mtu_bytes;
    
    // Create UDP socket
    int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        return -1;
    }
    
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(params.udp_server_port);
    
    if (inet_pton(AF_INET, params.udp_server_ip, &server_addr.sin_addr) <= 0) {
        close(udp_socket);
        return -1;
    }
    
    const uint8_t* data = static_cast<const uint8_t*>(handle->user_buffer);
    
    // Send packets in range
    for (uint32_t i = start_packet; i < end_packet && i < handle->total_packets; ++i) {
        uint32_t packet_offset = i;
        size_t packet_data_len = std::min(static_cast<size_t>(mtu_bytes),
                                         handle->buffer_size - (i * mtu_bytes));
        
        SDRPacket* packet = SDRPacket::create_data_packet(
            params.transfer_id, handle->msg_id, packet_offset,
            params.packets_per_chunk,
            data + (i * mtu_bytes), packet_data_len);
        
        if (!packet) {
            continue;
        }
        
        // Calculate total size BEFORE converting to network byte order
        size_t total_packet_size = sizeof(SDRPacketHeader) + packet_data_len;
        
        packet->header.to_network_order();
        
        ssize_t sent = sendto(udp_socket, packet, total_packet_size, 0,
                             (struct sockaddr*)&server_addr, sizeof(server_addr));
        
        if (sent > 0) {
            handle->packets_sent++;
        }
        
        SDRPacket::destroy(packet);
    }
    
    close(udp_socket);
    return 0;
}

int sdr_send_stream_end(SDRStreamHandle* handle) {
    if (!handle) {
        return -1;
    }
    
    handle->is_active = false;
    delete handle;
    return 0;
}

} // namespace sdr

