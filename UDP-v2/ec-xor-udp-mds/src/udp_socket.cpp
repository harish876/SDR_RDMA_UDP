#include "udp_socket.h"
#include <cerrno>
#include <iostream> 
#include <cstring> // for strerror

UDPSocket::UDPSocket() : sockfd_(-1) {
  sockfd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd_ < 0) {
    std::cerr << "socket() failed: " << strerror(errno) << std::endl;
  } else {
    int yes = 1;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  }
}

UDPSocket::~UDPSocket() {
  if (sockfd_ >= 0) {
    ::close(sockfd_);
  }
}

void UDPSocket::bind_socket(uint16_t port) {
  std::memset(&bind_addr_, 0, sizeof(bind_addr_));
  bind_addr_.sin_family = AF_INET;
  bind_addr_.sin_addr.s_addr = INADDR_ANY;
  bind_addr_.sin_port = htons(port);
  if (::bind(sockfd_, reinterpret_cast<sockaddr*>(&bind_addr_), sizeof(bind_addr_)) < 0) {
    std::cerr << "bind() port " << port << " failed: " << strerror(errno) << std::endl;
  }
}

void UDPSocket::set_peer(const std::string& ip, uint16_t port) {
  std::memset(&peer_, 0, sizeof(peer_));
  peer_.sin_family = AF_INET;
  peer_.sin_port   = htons(port);
  if (::inet_pton(AF_INET, ip.c_str(), &peer_.sin_addr) != 1) {
    std::cerr << "inet_pton failed for " << ip << std::endl;
  }
}

ssize_t UDPSocket::send_packet(const void* buf, std::size_t len) {
  return ::sendto(sockfd_, buf, len, 0, reinterpret_cast<sockaddr*>(&peer_), sizeof(peer_));
}

ssize_t UDPSocket::recv_bytes(void* buf, std::size_t len, sockaddr_in& src) {
  socklen_t sl = sizeof(src);
  return ::recvfrom(sockfd_, buf, len, 0, reinterpret_cast<sockaddr*>(&src), &sl);
}