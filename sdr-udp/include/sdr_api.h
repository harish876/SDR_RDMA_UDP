#pragma once

#include "tcp_control.h"
#include "sdr_connection.h"
#include "sdr_receiver.h"
#include "sdr_backend.h"
#include "sdr_frontend.h"
#include <cstdint>
#include <memory>
#include <string>

namespace sdr {

// Forward declarations
struct SDRContext;
struct SDRConnection;
struct SDRRecvHandle;
struct SDRSendHandle;
struct SDRStreamHandle;

// SDR Context - main initialization
struct SDRContext {
    // Internal state
    std::string device_name;
    uint32_t next_msg_id;
    std::mutex msg_id_mutex;
    
    SDRContext() : next_msg_id(0) {}
};

// Connection handle
struct SDRConnection {
    std::shared_ptr<ConnectionContext> connection_ctx;
    std::shared_ptr<UDPReceiver> udp_receiver;
    TCPControlServer* tcp_server;    // Owned by receiver side
    TCPControlClient* tcp_client;    // Owned by sender side
    bool is_receiver;                // true if receiver, false if sender
    
    SDRConnection() : tcp_server(nullptr), tcp_client(nullptr), is_receiver(false) {}
    
    ~SDRConnection() {
        if (tcp_server) delete tcp_server;
        if (tcp_client) delete tcp_client;
    }
};

// Receive handle
struct SDRRecvHandle {
    uint32_t msg_id;
    std::shared_ptr<MessageContext> msg_ctx;
    void* user_buffer;
    size_t buffer_size;
};

// Send handle (one-shot)
struct SDRSendHandle {
    uint32_t msg_id;
    std::shared_ptr<ConnectionContext> connection_ctx;
    const void* user_buffer;
    size_t buffer_size;
    size_t packets_sent;
};

// Stream handle (streaming send)
struct SDRStreamHandle {
    uint32_t msg_id;
    std::shared_ptr<ConnectionContext> connection_ctx;
    const void* user_buffer;
    size_t buffer_size;
    size_t total_packets;
    size_t packets_sent;
    bool is_active;
};

// Public API Functions

// Context management
SDRContext* sdr_ctx_create(const char* device_name);

void sdr_ctx_destroy(SDRContext* ctx);

// Connection establishment
SDRConnection* sdr_connect(SDRContext* ctx, const char* server_ip, uint16_t tcp_port);

SDRConnection* sdr_listen(SDRContext* ctx, uint16_t tcp_port);

void sdr_disconnect(SDRConnection* conn);

// Connection parameter configuration (set before recv_post/send_post)
int sdr_set_params(SDRConnection* conn, const ConnectionParams* params);

// Receive operations
int sdr_recv_post(SDRConnection* conn, void* buffer, size_t length, SDRRecvHandle** handle);

int sdr_recv_bitmap_get(SDRRecvHandle* handle, const uint8_t** bitmap, size_t* len);

int sdr_recv_complete(SDRRecvHandle* handle);

// Send operations (one-shot)
int sdr_send_post(SDRConnection* conn, const void* buffer, size_t length, SDRSendHandle** handle);

int sdr_send_poll(SDRSendHandle* handle);

// Send operations (streaming)
int sdr_send_stream_start(SDRConnection* conn, const void* buffer, size_t length, 
                          uint32_t initial_offset, SDRStreamHandle** handle);

int sdr_send_stream_continue(SDRStreamHandle* handle, uint32_t offset, size_t length);

int sdr_send_stream_end(SDRStreamHandle* handle);

} // namespace sdr


