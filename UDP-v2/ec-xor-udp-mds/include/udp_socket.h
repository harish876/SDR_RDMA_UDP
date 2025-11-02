#pragma once
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <cstring>

class UDPSocket {
  int         sockfd_;
  sockaddr_in peer_{};
  sockaddr_in bind_addr_{};
public:
  UDPSocket();
  ~UDPSocket();
  void bind_socket(uint16_t port);
  void set_peer(const std::string& ip, uint16_t port);
  ssize_t send_packet(const void* buf, std::size_t len);
  ssize_t recv_bytes(void* buf, std::size_t len, sockaddr_in& src);
  int fd() const { return sockfd_; }
};