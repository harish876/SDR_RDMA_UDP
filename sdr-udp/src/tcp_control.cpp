#include "tcp_control.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <cstring>

namespace sdr {

// ControlMessage serialization
size_t ControlMessage::serialize(uint8_t* buffer, size_t buffer_size) const {
    if (buffer_size < sizeof(ControlMessage)) {
        return 0;
    }
    
    // Use memcpy for proper alignment
    std::memcpy(buffer, this, sizeof(ControlMessage));
    return sizeof(ControlMessage);
}

bool ControlMessage::deserialize(const uint8_t* buffer, size_t buffer_size) {
    if (buffer_size < sizeof(ControlMessage)) {
        return false;
    }
    
    std::memcpy(this, buffer, sizeof(ControlMessage));
    
    // Validate magic
    if (magic != MAGIC_VALUE) {
        return false;
    }
    
    return true;
}

// TCPControlServer implementation
TCPControlServer::TCPControlServer() 
    : listen_fd_(-1), client_fd_(-1), listen_port_(0), is_listening_(false) {
}

TCPControlServer::~TCPControlServer() {
    stop();
}

bool TCPControlServer::start_listening(uint16_t port) {
    if (is_listening_) {
        return false;
    }
    
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "[TCP Server] Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "[TCP Server] setsockopt failed: " << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    
    // Bind
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(listen_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "[TCP Server] Bind failed: " << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    
    // Listen
    if (listen(listen_fd_, 1) < 0) {
        std::cerr << "[TCP Server] Listen failed: " << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    
    listen_port_ = port;
    is_listening_ = true;
    std::cout << "[TCP Server] Listening on port " << port << std::endl;
    return true;
}

bool TCPControlServer::accept_connection() {
    if (!is_listening_) {
        return false;
    }
    
    if (client_fd_ >= 0) {
        close(client_fd_);
    }
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    std::cout << "[TCP Server] Waiting for client connection..." << std::endl;
    client_fd_ = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_fd_ < 0) {
        std::cerr << "[TCP Server] Accept failed: " << strerror(errno) << std::endl;
        return false;
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::cout << "[TCP Server] Client connected from " << client_ip << ":" 
              << ntohs(client_addr.sin_port) << std::endl;
    
    return true;
}

bool TCPControlServer::receive_message(ControlMessage& msg) {
    if (client_fd_ < 0) {
        return false;
    }
    
    uint8_t buffer[sizeof(ControlMessage)];
    ssize_t total_received = 0;
    
    // Receive entire message
    while (total_received < sizeof(ControlMessage)) {
        ssize_t n = recv(client_fd_, buffer + total_received, 
                        sizeof(ControlMessage) - total_received, 0);
        
        if (n <= 0) {
            if (n == 0) {
                std::cerr << "[TCP Server] Client disconnected" << std::endl;
            } else {
                std::cerr << "[TCP Server] Recv failed: " << strerror(errno) << std::endl;
            }
            close(client_fd_);
            client_fd_ = -1;
            return false;
        }
        
        total_received += n;
    }
    
    return msg.deserialize(buffer, sizeof(ControlMessage));
}

bool TCPControlServer::send_message(const ControlMessage& msg) {
    if (client_fd_ < 0) {
        return false;
    }
    
    uint8_t buffer[sizeof(ControlMessage)];
    size_t len = msg.serialize(buffer, sizeof(buffer));
    
    ssize_t sent = send(client_fd_, buffer, len, 0);
    if (sent < 0) {
        std::cerr << "[TCP Server] Send failed: " << strerror(errno) << std::endl;
        return false;
    }
    
    if (static_cast<size_t>(sent) != len) {
        std::cerr << "[TCP Server] Partial send: " << sent << "/" << len << std::endl;
        return false;
    }
    
    return true;
}

void TCPControlServer::close_connection() {
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }
}

void TCPControlServer::stop() {
    close_connection();
    
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    
    is_listening_ = false;
    listen_port_ = 0;
}

// TCPControlClient implementation
TCPControlClient::TCPControlClient() 
    : socket_fd_(-1), is_connected_(false) {
}

TCPControlClient::~TCPControlClient() {
    disconnect();
}

bool TCPControlClient::connect_to_server(const std::string& server_ip, uint16_t server_port) {
    if (is_connected_) {
        disconnect();
    }
    
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        std::cerr << "[TCP Client] Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "[TCP Client] Invalid server IP address: " << server_ip << std::endl;
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    std::cout << "[TCP Client] Connecting to " << server_ip << ":" << server_port << std::endl;
    if (connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "[TCP Client] Connect failed: " << strerror(errno) << std::endl;
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    is_connected_ = true;
    std::cout << "[TCP Client] Connected successfully" << std::endl;
    return true;
}

bool TCPControlClient::send_message(const ControlMessage& msg) {
    if (!is_connected_) {
        return false;
    }
    
    uint8_t buffer[sizeof(ControlMessage)];
    size_t len = msg.serialize(buffer, sizeof(buffer));
    
    ssize_t sent = send(socket_fd_, buffer, len, 0);
    if (sent < 0) {
        std::cerr << "[TCP Client] Send failed: " << strerror(errno) << std::endl;
        is_connected_ = false;
        return false;
    }
    
    if (static_cast<size_t>(sent) != len) {
        std::cerr << "[TCP Client] Partial send: " << sent << "/" << len << std::endl;
        return false;
    }
    
    return true;
}

bool TCPControlClient::receive_message(ControlMessage& msg) {
    if (!is_connected_) {
        return false;
    }
    
    uint8_t buffer[sizeof(ControlMessage)];
    ssize_t total_received = 0;
    
    // Receive entire message
    while (total_received < sizeof(ControlMessage)) {
        ssize_t n = recv(socket_fd_, buffer + total_received, 
                        sizeof(ControlMessage) - total_received, 0);
        
        if (n <= 0) {
            if (n == 0) {
                std::cerr << "[TCP Client] Server disconnected" << std::endl;
            } else {
                std::cerr << "[TCP Client] Recv failed: " << strerror(errno) << std::endl;
            }
            is_connected_ = false;
            return false;
        }
        
        total_received += n;
    }
    
    return msg.deserialize(buffer, sizeof(ControlMessage));
}

void TCPControlClient::disconnect() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    is_connected_ = false;
}

} // namespace sdr

