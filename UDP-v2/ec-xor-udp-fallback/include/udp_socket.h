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

  // Receiver: bind to a local port
  void bind_socket(uint16_t port);

  // Sender: set destination IP/port
  void set_peer(const std::string& ip, uint16_t port);

  // Send typed packet (data path)
  ssize_t send_packet(const void* buf, std::size_t len);

  // Receive into buffer; fills src
  ssize_t recv_bytes(void* buf, std::size_t len, sockaddr_in& src);

  // Access raw fd (for sendto when you have dynamic dest)
  int fd() const { return sockfd_; }
};