#include "udp_socket.h"
#include "utils.h"
#include "mds_ec.h"
#include <iostream>
#include <vector>
#include <map>
#include <cstring>
#include <chrono>
#include <cerrno>
#include <thread> 

using namespace std;

struct GroupState {
  PacketGroup packets;
  int received_count = 0;
  bool is_recovered = false;

  GroupState() : packets(EC_GROUP_SIZE) {
    for (auto& pkt : packets) {
      pkt.data_size = 0;
    }
  }
};

int main(int argc, char* argv[]) {
  if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <listen_port>\n";
    return 1;
  }
  
  MDSErasureCoding::init(); // Initialize MDS library

  int listen_port = stoi(argv[1]);

  UDPSocket sock;
  sock.bind_socket(listen_port);

  // Set a 500ms non-blocking timeout
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 500000; // 500ms
  if (setsockopt(sock.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      perror("Error setting socket timeout");
      return 1;
  }

  cout << "[EC Receiver] Listening on port " << listen_port << endl;
  cout << "[EC Receiver] Expecting " << TOTAL_GROUPS << " groups." << endl;

  map<uint32_t, GroupState> groups;
  int groups_fully_recovered = 0;
  bool sender_addr_known = false;
  sockaddr_in sender_addr{};

  auto last_packet_time = chrono::steady_clock::now();
  const auto FTO_DURATION = chrono::seconds(2);

  while (groups_fully_recovered < TOTAL_GROUPS) {
    ECPacket buffer;
    ssize_t n = sock.recv_bytes(&buffer, sizeof(buffer), sender_addr);

    if (n == sizeof(ECPacket)) {
      if (!sender_addr_known) {
        sender_addr_known = true;
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, ipbuf, sizeof(ipbuf));
        cout << "[EC Receiver] Learned sender address: " << ipbuf << ":" << ntohs(sender_addr.sin_port) << endl;
      }
      
      last_packet_time = chrono::steady_clock::now(); 
      uint32_t gid = buffer.group_id;

      if (groups.find(gid) == groups.end()) {
        groups[gid] = GroupState(); 
      }

      if (groups[gid].is_recovered) {
        ECPacket ack_pkt;
        ack_pkt.group_id = gid;
        ack_pkt.type = GROUP_ACK;
        ack_pkt.data_size = 0;
        ::sendto(sock.fd(), &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)&sender_addr, sizeof(sender_addr));
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
        continue;
      }

      int slot_index = -1;
      if (buffer.type == DATA_CHUNK && buffer.chunk_index < EC_DATA_CHUNKS_K) {
        slot_index = buffer.chunk_index;
      } else if (buffer.type == PARITY_CHUNK && buffer.chunk_index < EC_PARITY_CHUNKS_M) {
        slot_index = EC_DATA_CHUNKS_K + buffer.chunk_index;
      } else {
        continue; 
      }

      if (groups[gid].packets[slot_index].data_size == 0) {
        groups[gid].packets[slot_index] = buffer;
        groups[gid].received_count++;
      }

      // We have 'm' parity chunks, so we only *need* 'k' total chunks to recover.
      if (groups[gid].received_count >= EC_DATA_CHUNKS_K) { 
        if (MDSErasureCoding::decode(groups[gid].packets)) {
          groups[gid].is_recovered = true;
          groups_fully_recovered++;
          cout << "[EC Receiver] ✅ Group " << gid << " successfully recovered! ("
               << groups_fully_recovered << "/" << TOTAL_GROUPS << ")" << endl;

          ECPacket ack_pkt;
          ack_pkt.group_id = gid;
          ack_pkt.type = GROUP_ACK;
          ack_pkt.data_size = 0;
          ::sendto(sock.fd(), &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)&sender_addr, sizeof(sender_addr));
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      auto now = chrono::steady_clock::now();
      if (chrono::duration_cast<chrono::milliseconds>(now - last_packet_time) > FTO_DURATION) {
        if (!sender_addr_known) continue; 

        cout << "[EC Receiver] ❌ Fallback Timer expired. Sending NACKs for failed groups." << endl;
        
        for (auto& pair : groups) {
          uint32_t gid = pair.first;
          GroupState& state = pair.second;

          if (!state.is_recovered) { 
            ECPacket nack_pkt;
            nack_pkt.group_id = gid;
            nack_pkt.type = NACK;
            nack_pkt.data_size = EC_DATA_CHUNKS_K; 
            std::memset(nack_pkt.payload, 0, CHUNK_PAYLOAD_SIZE);

            for (int k = 0; k < EC_DATA_CHUNKS_K; ++k) {
              if (state.packets[k].data_size == 0) {
                nack_pkt.payload[k] = 1; // 1 means "I am missing this"
              }
            }
            
            cout << "[EC Receiver] Sending NACK for group " << gid << endl;
            ::sendto(sock.fd(), &nack_pkt, sizeof(nack_pkt), 0, (struct sockaddr*)&sender_addr, sizeof(sender_addr));
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
          }
        }
        last_packet_time = now; 
      }
    }
  } // while

  cout << "[EC Receiver] ✅✅ All " << TOTAL_GROUPS << " groups recovered. Transfer complete." << endl;
  return 0;
}