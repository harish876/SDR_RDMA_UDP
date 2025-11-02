#include "udp_socket.h"
#include "utils.h"
#include "xor_ec.h"
#include <iostream>
#include <vector>
#include <map>
#include <cstring>
#include <chrono>
#include <cerrno>
#include <thread> // <-- 1. ADDED THIS INCLUDE

using namespace std;

// This struct will hold the state for each group we are receiving
struct GroupState {
  PacketGroup packets;
  int received_count = 0;
  bool is_recovered = false;
  bool nack_sent = false; // <-- 2. ADDED THIS FLAG

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

    // --- HANDLE RECEIVE ---
    if (n == sizeof(ECPacket)) {
      if (!sender_addr_known) {
        sender_addr_known = true;
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, ipbuf, sizeof(ipbuf));
        cout << "[EC Receiver] Learned sender address: " << ipbuf << ":" << ntohs(sender_addr.sin_port) << endl;
      }
      
      last_packet_time = chrono::steady_clock::now(); // Reset timer
      uint32_t gid = buffer.group_id;

      if (groups.find(gid) == groups.end()) {
        groups[gid] = GroupState(); 
      }

      // --- 3. FIX FOR LOST ACKS ---
      if (groups[gid].is_recovered) {
        // This is a retransmission for a group we've already fixed.
        // Our ACK must have been lost. Let's re-send it.
        ECPacket ack_pkt;
        ack_pkt.group_id = gid;
        ack_pkt.type = GROUP_ACK;
        ack_pkt.data_size = 0;
        ::sendto(sock.fd(), &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)&sender_addr, sizeof(sender_addr));
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Pace the re-ACK
        continue;
      }
      // --- END OF FIX ---

      int slot_index = -1;
      if (buffer.type == DATA_CHUNK && buffer.chunk_index < EC_DATA_CHUNKS_K) {
        slot_index = buffer.chunk_index;
      } else if (buffer.type == PARITY_CHUNK && buffer.chunk_index < EC_PARITY_CHUNKS_M) {
        slot_index = EC_DATA_CHUNKS_K + buffer.chunk_index;
      } else {
        continue; // Not a data/parity packet
      }

      if (groups[gid].packets[slot_index].data_size == 0) {
        groups[gid].packets[slot_index] = buffer;
        groups[gid].received_count++;
      }

      // --- TRY TO DECODE AND SEND ACK ---
      if (groups[gid].received_count >= EC_DATA_CHUNKS_K) {
        if (XORErasureCoding::decode(groups[gid].packets)) {
          groups[gid].is_recovered = true;
          groups_fully_recovered++;
          cout << "[EC Receiver] ✅ Group " << gid << " successfully recovered! ("
               << groups_fully_recovered << "/" << TOTAL_GROUPS << ")" << endl;

          // Send ACK back to sender
          ECPacket ack_pkt;
          ack_pkt.group_id = gid;
          ack_pkt.type = GROUP_ACK;
          ack_pkt.data_size = 0;
          ::sendto(sock.fd(), &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr*)&sender_addr, sizeof(sender_addr));
        }
      }
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // --- THIS IS A TIMEOUT ---
      auto now = chrono::steady_clock::now();
      if (chrono::duration_cast<chrono::milliseconds>(now - last_packet_time) > FTO_DURATION) {
        if (!sender_addr_known) continue; // Can't send NACKs if we don't know who to send to

        cout << "[EC Receiver] ❌ Fallback Timer expired. Sending NACKs for failed groups." << endl;
        
        for (auto& pair : groups) {
          uint32_t gid = pair.first;
          GroupState& state = pair.second;

          // --- 4. FIX FOR NACK STORM ---
          if (!state.is_recovered && !state.nack_sent) { // Only NACK if we haven't already
            // This group failed. Send a NACK.
            ECPacket nack_pkt;
            nack_pkt.group_id = gid;
            nack_pkt.type = NACK;
            nack_pkt.data_size = EC_DATA_CHUNKS_K; // Signifies payload is a bitmap
            std::memset(nack_pkt.payload, 0, CHUNK_PAYLOAD_SIZE);

            for (int k = 0; k < EC_DATA_CHUNKS_K; ++k) {
              if (state.packets[k].data_size == 0) {
                nack_pkt.payload[k] = 1; // 1 means "I am missing this"
              }
            }
            
            cout << "[EC Receiver] Sending NACK for group " << gid << endl;
            ::sendto(sock.fd(), &nack_pkt, sizeof(nack_pkt), 0, (struct sockaddr*)&sender_addr, sizeof(sender_addr));
            state.nack_sent = true; // Mark as sent
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Pace the NACKs
          }
          // --- END OF FIX ---
        }
        last_packet_time = now; // Reset timer
      }
    }
  } // while

  cout << "[EC Receiver] ✅✅ All " << TOTAL_GROUPS << " groups recovered. Transfer complete." << endl;
  return 0;
}