#include "sdr_api.h"
#include "sdr_packet.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

namespace sdr {

namespace {
// Allocate a message ID from the 10-bit space (0-1023) with wraparound.
uint32_t allocate_msg_id(SDRContext* ctx) {
    std::lock_guard<std::mutex> lock(ctx->msg_id_mutex);
    uint32_t id = ctx->next_msg_id % 1024;
    ctx->next_msg_id = (ctx->next_msg_id + 1) % 1024;
    return id;
}
} // namespace

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
    conn->parent_ctx = ctx;
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
    conn->parent_ctx = ctx;
    conn->is_receiver = false;
    conn->tcp_client = new TCPControlClient();
    
    if (!conn->tcp_client->connect_to_server(server_ip, tcp_port)) {
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

int sdr_set_params(SDRConnection* conn, const ConnectionParams* params) {
    if (!conn || !params) {
        return -1;
    }
    
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
    
    // Wait for OFFER from sender
    ControlMessage offer{};
    while (true) {
        if (!conn->tcp_server->receive_message(offer)) {
            std::cerr << "[SDR API] Failed to receive OFFER" << std::endl;
            return -1;
        }
        if (offer.msg_type == ControlMsgType::OFFER) break;
        std::cerr << "[SDR API] Skipping unexpected control message type " << static_cast<int>(offer.msg_type) << std::endl;
    }
    
    // Allocate message ID
    uint32_t msg_id = allocate_msg_id(conn->parent_ctx);
    
    // Get current connection parameters and negotiate with offer
    ConnectionParams params = conn->connection_ctx->get_params();
    params.total_bytes = offer.params.total_bytes ? offer.params.total_bytes : length;
    uint32_t proposed_mtu = offer.params.mtu_bytes ? offer.params.mtu_bytes : params.mtu_bytes;
    if (proposed_mtu == 0) proposed_mtu = SDRPacket::MAX_PAYLOAD_SIZE;
    params.mtu_bytes = std::min<uint32_t>(proposed_mtu, SDRPacket::MAX_PAYLOAD_SIZE);
    params.packets_per_chunk = offer.params.packets_per_chunk ? offer.params.packets_per_chunk
                                                              : (params.packets_per_chunk ? params.packets_per_chunk : 64);
    params.num_channels = offer.params.num_channels ? offer.params.num_channels
                                                    : (params.num_channels ? params.num_channels : 1);
    if (params.udp_server_port == 0) {
        params.udp_server_port = params.channel_base_port ? params.channel_base_port : 9999;
    }
    params.channel_base_port = params.channel_base_port ? params.channel_base_port : params.udp_server_port;
    if (params.udp_server_ip[0] == '\0') {
        std::strncpy(params.udp_server_ip, "127.0.0.1", sizeof(params.udp_server_ip) - 1);
        params.udp_server_ip[sizeof(params.udp_server_ip) - 1] = '\0';
    }
    if (params.transfer_id == 0) {
        params.transfer_id = 1;
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
    // Use transfer_id as generation; if unset, generate monotonically
    static std::atomic<uint32_t> receiver_generation_counter{1};
    uint32_t generation = receiver_generation_counter.fetch_add(1, std::memory_order_relaxed);
    params.transfer_id = generation;
    
    MessageContext* msg_ctx = conn->connection_ctx->allocate_message_slot(msg_id, generation);
    
    if (!msg_ctx) {
        std::cerr << "[SDR API] Failed to allocate message slot (msg_id=" << msg_id 
                  << ", generation=" << generation << ")" << std::endl;
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
    recv_handle->generation = generation;
    recv_handle->msg_ctx = std::shared_ptr<MessageContext>(msg_ctx, [](MessageContext*) {}); // Non-owning
    recv_handle->user_buffer = buffer;
    recv_handle->buffer_size = length;
    recv_handle->conn = conn;  // Store connection reference for ACK
    *handle = recv_handle;
    
    // Start UDP receiver if not already started
    if (!conn->udp_receiver) {
        conn->udp_receiver = std::make_shared<UDPReceiver>(conn->connection_ctx);
        if (!conn->udp_receiver->start(params.channel_base_port, params.num_channels)) {
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

    // Wait for ACCEPT from sender
    ControlMessage accept{};
    while (true) {
        if (!conn->tcp_server->receive_message(accept)) {
            std::cerr << "[SDR API] Failed to receive ACCEPT" << std::endl;
            return -1;
        }
        if (accept.msg_type == ControlMsgType::ACCEPT) break;
        std::cerr << "[SDR API] Skipping unexpected control message type " << static_cast<int>(accept.msg_type) << std::endl;
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
    
    const std::atomic<uint64_t>* chunk_bitmap = handle->msg_ctx->frontend_bitmap->get_chunk_bitmap();
    uint32_t num_words = handle->msg_ctx->frontend_bitmap->get_chunk_bitmap_size();
    
    *len = num_words * sizeof(uint64_t);
    *bitmap = reinterpret_cast<const uint8_t*>(chunk_bitmap);
    
    return 0;
}

int sdr_recv_complete(SDRRecvHandle* handle) {
    if (!handle || !handle->msg_ctx) {
        return -1;
    }
    
    if (handle->msg_ctx->frontend_bitmap) {
        handle->msg_ctx->frontend_bitmap->stop_polling();
    }
    
    // Check if transfer is actually complete before sending ACK
    bool is_complete = false;
    if (handle->msg_ctx->frontend_bitmap) {
        uint32_t chunks_received = handle->msg_ctx->frontend_bitmap->get_total_chunks_completed();
        uint32_t total_chunks = handle->msg_ctx->total_chunks;
        is_complete = (total_chunks > 0 && chunks_received >= total_chunks);
    }
    
    // Mark message as completed and protect against late packets
    if (handle->conn && handle->conn->connection_ctx) {
        handle->conn->connection_ctx->complete_message(handle->msg_id);
    } else {
        handle->msg_ctx->state = MessageState::DEAD;
    }
    
    // Send completion ACK or NACK to sender
    if (handle->conn && handle->conn->is_receiver && handle->conn->tcp_server) {
        ControlMessage ack_msg;
        ack_msg.magic = ControlMessage::MAGIC_VALUE;
        ack_msg.msg_type = is_complete ? ControlMsgType::COMPLETE_ACK : ControlMsgType::INCOMPLETE_NACK;
        ack_msg.connection_id = handle->conn->connection_ctx->get_connection_id();
        std::memset(&ack_msg.params, 0, sizeof(ack_msg.params));
        
        if (handle->conn->tcp_server->send_message(ack_msg)) {
            if (is_complete) {
                std::cout << "[SDR API] Sent completion ACK to sender" << std::endl;
            } else {
                std::cout << "[SDR API] Sent incomplete NACK to sender (transfer incomplete)" << std::endl;
            }
        } else {
            std::cerr << "[SDR API] Failed to send completion message" << std::endl;
        }
    }
    
    return 0;
}

int sdr_send_post(SDRConnection* conn, const void* buffer, size_t length, SDRSendHandle** handle) {
    if (!conn || !buffer || length == 0 || !handle) {
        return -1;
    }
    
    if (conn->is_receiver) {
        return -1;
    }
    
    if (!conn->tcp_client || !conn->tcp_client->is_connected()) {
        return -1;
    }
    
    // Propose parameters via OFFER
    ControlMessage offer{};
    offer.magic = ControlMessage::MAGIC_VALUE;
    offer.msg_type = ControlMsgType::OFFER;
    offer.connection_id = conn->connection_ctx->get_connection_id();
    ConnectionParams desired{};
    desired.total_bytes = length;
    desired.mtu_bytes = SDRPacket::MAX_PAYLOAD_SIZE;
    desired.packets_per_chunk = 32;
    desired.num_channels = 1;
    desired.channel_base_port = 0;
    desired.udp_server_port = 0;
    std::memset(desired.udp_server_ip, 0, sizeof(desired.udp_server_ip));
    offer.params = desired;
    conn->tcp_client->send_message(offer);

    ControlMessage cts_msg;
    // Loop until we get a CTS (skip any stale control messages)
    while (true) {
        if (!conn->tcp_client->receive_message(cts_msg)) {
            std::cerr << "[SDR API] Failed to receive CTS" << std::endl;
            return -1;
        }
        if (cts_msg.msg_type == ControlMsgType::CTS) break;
        std::cerr << "[SDR API] Skipping unexpected control message type " << static_cast<int>(cts_msg.msg_type) << std::endl;
    }
    
    conn->connection_ctx->initialize(cts_msg.connection_id, cts_msg.params);

    // Send ACCEPT back to receiver
    ControlMessage accept{};
    accept.magic = ControlMessage::MAGIC_VALUE;
    accept.msg_type = ControlMsgType::ACCEPT;
    accept.connection_id = cts_msg.connection_id;
    accept.params = cts_msg.params;
    conn->tcp_client->send_message(accept);
    
    if (cts_msg.params.mtu_bytes == 0) {
        std::cerr << "[SDR API] Error: MTU bytes is zero in CTS message" << std::endl;
        return -1;
    }
    if (cts_msg.params.packets_per_chunk == 0) {
        std::cerr << "[SDR API] Error: Packets per chunk is zero in CTS message" << std::endl;
        return -1;
    }
    
    uint32_t msg_id = allocate_msg_id(conn->parent_ctx);
    
    auto* send_handle = new SDRSendHandle();
    send_handle->msg_id = msg_id;
    send_handle->generation = cts_msg.params.transfer_id;
    send_handle->connection_ctx = conn->connection_ctx;
    send_handle->user_buffer = buffer;
    send_handle->buffer_size = length;
    send_handle->packets_sent = 0;
    send_handle->conn = conn;  // Store connection reference for ACK
    *handle = send_handle;
    std::strncpy(cts_msg.params.udp_server_ip, "130.127.134.60", sizeof(cts_msg.params.udp_server_ip) - 1);
    cts_msg.params.udp_server_ip[sizeof(cts_msg.params.udp_server_ip) - 1] = '\0'; 
    uint32_t mtu_bytes = cts_msg.params.mtu_bytes;
    size_t total_packets = (length + mtu_bytes - 1) / mtu_bytes;
    size_t expected_total_packets = (cts_msg.params.total_bytes + mtu_bytes - 1) / mtu_bytes;
    if (cts_msg.params.total_bytes != 0 && total_packets != expected_total_packets) {
        std::cerr << "[SDR API] Warning: sender length (" << length << ") does not match receiver expectation ("
                  << cts_msg.params.total_bytes << ")" << std::endl;
    }
    
    std::cout << "[SDR API] Sending " << total_packets << " packets (MTU: " << mtu_bytes 
              << ", packets_per_chunk: " << cts_msg.params.packets_per_chunk << ")" << std::endl;
    
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
    
    uint16_t num_channels = cts_msg.params.num_channels == 0 ? 1 : cts_msg.params.num_channels;
    uint16_t base_port = cts_msg.params.channel_base_port == 0 ? cts_msg.params.udp_server_port
                                                               : cts_msg.params.channel_base_port;
    
    std::cout << "[SDR API] Sending to " << cts_msg.params.udp_server_ip 
              << " base port " << base_port << " across " << num_channels << " channel(s)" << std::endl;
    
    size_t packets_failed = 0;
    if (conn->connection_ctx->auto_send_data()) {
        for (size_t i = 0; i < total_packets; ++i) {
            uint32_t packet_offset = static_cast<uint32_t>(i);
            size_t remaining = length - (i * mtu_bytes);
            size_t packet_data_len = std::min(static_cast<size_t>(mtu_bytes), remaining);
            
            // Ensure packet_data_len doesn't exceed MAX_PAYLOAD_SIZE
            if (packet_data_len > SDRPacket::MAX_PAYLOAD_SIZE) {
                packet_data_len = SDRPacket::MAX_PAYLOAD_SIZE;
            }
            
            SDRPacket* packet = SDRPacket::create_data_packet(
                cts_msg.params.transfer_id, msg_id, packet_offset,
                cts_msg.params.packets_per_chunk,
                data + (i * mtu_bytes), packet_data_len);
            if (packet) {
                packet->header.chunk_seq = packet->header.get_chunk_id();
            }
            
            if (!packet) {
                std::cerr << "[SDR API] Failed to create packet " << i 
                          << " (data_len: " << packet_data_len << ", MAX: " 
                          << SDRPacket::MAX_PAYLOAD_SIZE << ")" << std::endl;
                packets_failed++;
                continue;
            }
            
            size_t total_packet_size = sizeof(SDRPacketHeader) + packet_data_len;
            
            packet->header.to_network_order();
            
            uint16_t channel_port = base_port + static_cast<uint16_t>(i % num_channels);
            server_addr.sin_port = htons(channel_port);
            ssize_t sent = sendto(udp_socket, packet, total_packet_size, 0,
                                 (struct sockaddr*)&server_addr, sizeof(server_addr));
            
            if (sent > 0) {
                send_handle->packets_sent++;
            } else {
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
    }
    
    close(udp_socket);
    return 0;
}

int sdr_send_poll(SDRSendHandle* handle) {
    if (!handle) {
        return -1;
    }
    if (handle->conn && handle->conn->tcp_client && handle->conn->tcp_client->is_connected()) {
        ControlMessage ack_msg;
        std::cout << "[SDR API] Waiting for completion ACK from receiver..." << std::endl;
        
        if (!handle->conn->tcp_client->receive_message(ack_msg)) {
            std::cerr << "[SDR API] Failed to receive completion ACK" << std::endl;
            return -1;
        }
        
        if (ack_msg.msg_type == ControlMsgType::COMPLETE_ACK) {
            std::cout << "[SDR API] Received completion ACK from receiver - transfer successful!" << std::endl;
            return 0;
        } else if (ack_msg.msg_type == ControlMsgType::INCOMPLETE_NACK) {
            std::cerr << "[SDR API] Received incomplete NACK - receiver did not complete transfer (packet loss or timeout)" << std::endl;
            return -1;
        } else {
            std::cerr << "[SDR API] Expected COMPLETE_ACK or INCOMPLETE_NACK, got message type: " 
                      << static_cast<int>(ack_msg.msg_type) << std::endl;
            return -1;
        }
    }
    
    return 0;
}

int sdr_send_stream_start(SDRConnection* conn, const void* buffer, size_t length,
                         uint32_t initial_offset, SDRStreamHandle** handle) {
    if (!conn || !buffer || !handle) {
        return -1;
    }
    
    if (conn->is_receiver) {
        return -1; 
    }
    
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
    
    auto* stream_handle = new SDRStreamHandle();
    stream_handle->msg_id = allocate_msg_id(conn->parent_ctx);
    stream_handle->generation = cts_msg.params.transfer_id;
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
    
    const ConnectionParams& params = handle->connection_ctx->get_params();
    uint32_t mtu_bytes = params.mtu_bytes;
    
    uint32_t start_packet = offset / mtu_bytes;
    uint32_t end_packet = (offset + length + mtu_bytes - 1) / mtu_bytes;
    
    int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        return -1;
    }
    
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    uint16_t num_channels = params.num_channels == 0 ? 1 : params.num_channels;
    uint16_t base_port = params.channel_base_port == 0 ? params.udp_server_port : params.channel_base_port;
    
    if (inet_pton(AF_INET, params.udp_server_ip, &server_addr.sin_addr) <= 0) {
        close(udp_socket);
        return -1;
    }
    
    const uint8_t* data = static_cast<const uint8_t*>(handle->user_buffer);
    
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
        packet->header.chunk_seq = packet->header.get_chunk_id();
        
        size_t total_packet_size = sizeof(SDRPacketHeader) + packet_data_len;
        
        packet->header.to_network_order();
        
        uint16_t channel_port = base_port + static_cast<uint16_t>(i % num_channels);
        server_addr.sin_port = htons(channel_port);
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
