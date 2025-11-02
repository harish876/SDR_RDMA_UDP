#include "udp_socket.h"
#include "utils.h"
#include "mds_ec.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring> 
#include <chrono>
#include <thread>
#include <map>
#include <cerrno>
#include <fcntl.h> 

using namespace std;

// We need to re-generate data, so let's make a function
void generate_data_chunk(int group_id, int chunk_index, ECPacket& out_pkt) {
    out_pkt.group_id = group_id;
    out_pkt.type = DATA_CHUNK;
    out_pkt.chunk_index = chunk_index;
    out_pkt.data_size = CHUNK_PAYLOAD_SIZE;
    
    // Fill with dummy data
    char start_char = ((group_id * EC_DATA_CHUNKS_K + chunk_index) % 26) + 'A';
    std::memset(out_pkt.payload, start_char, CHUNK_PAYLOAD_SIZE);
}


int main(int argc, char* argv[]) {
  if (argc != 3) {
    cerr << "Usage: " << argv[0] << " <receiver_ip> <receiver_port>\n";
    return 1;
  }

  MDSErasureCoding::init(); // Initialize MDS library

  string receiver_ip = argv[1];
  int receiver_port = stoi(argv[2]);
  const int SENDER_PORT = 8000; // Port we will listen on

  UDPSocket sock;
  sock.bind_socket(SENDER_PORT); 
  sock.set_peer(receiver_ip, receiver_port);

  // MAKE THE SOCKET NON-BLOCKING
  int flags = fcntl(sock.fd(), F_GETFL, 0);
  if (flags == -1) { 
      perror("fcntl F_GETFL"); return 1; 
  }
  if (fcntl(sock.fd(), F_SETFL, flags | O_NONBLOCK) == -1) {
      perror("fcntl F_SETFL O_NONBLOCK"); return 1;
  }

  cout << "[EC Sender] Starting to send " << TOTAL_GROUPS << " groups to "
       << receiver_ip << ":" << receiver_port << endl;

  auto start = chrono::high_resolution_clock::now();

  for (int g = 0; g < TOTAL_GROUPS; ++g) {
    PacketGroup data_to_encode(EC_DATA_CHUNKS_K);
    for (int k = 0; k < EC_DATA_CHUNKS_K; ++k) {
      generate_data_chunk(g, k, data_to_encode[k]);
    }
    
    // Create 'm' empty parity packets
    PacketGroup parity_packets(EC_PARITY_CHUNKS_M);
    for (int m = 0; m < EC_PARITY_CHUNKS_M; ++m) {
        parity_packets[m].group_id = g;
        parity_packets[m].type = PARITY_CHUNK;
        parity_packets[m].chunk_index = m;
        parity_packets[m].data_size = CHUNK_PAYLOAD_SIZE;
    }

    MDSErasureCoding::encode(data_to_encode, parity_packets);

    // Pace the initial send
    for (const auto& pkt : data_to_encode) {
      sock.send_packet(&pkt, sizeof(ECPacket));
      std::this_thread::sleep_for(std::chrono::milliseconds(1)); 
    }
    for (const auto& pkt : parity_packets) {
      sock.send_packet(&pkt, sizeof(ECPacket));
      std::this_thread::sleep_for(std::chrono::milliseconds(1)); 
    }

    if ((g + 1) % (TOTAL_GROUPS / 4) == 0) {
      cout << "[EC Sender] Sent group " << g + 1 << "/" << TOTAL_GROUPS << endl;
    }
  }

  auto end_send = chrono::high_resolution_clock::now();
  auto duration_send = chrono::duration_cast<chrono::milliseconds>(end_send - start).count();
  cout << "[EC Sender] ✅ Initial send complete (" << duration_send << " ms)." << endl;
  cout << "[EC Sender] Now listening for ACKs/NACKs on port " << SENDER_PORT << "..." << endl;

  
  map<uint32_t, bool> groups_acked;
  int retransmissions = 0;
  int consecutive_timeouts = 0;
  const int MAX_CONSECUTIVE_TIMEOUTS = 5; 

  fd_set readfds;
  struct timeval tv;

  while (groups_acked.size() < TOTAL_GROUPS) {
    FD_ZERO(&readfds);
    FD_SET(sock.fd(), &readfds);

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    int activity = select(sock.fd() + 1, &readfds, nullptr, nullptr, &tv);

    if (activity > 0 && FD_ISSET(sock.fd(), &readfds)) {
        
        std::vector<ECPacket> received_acks;
        std::vector<ECPacket> received_nacks;
        
        while (true) {
            ECPacket buffer;
            sockaddr_in from_addr{}; 
            ssize_t n = sock.recv_bytes(&buffer, sizeof(buffer), from_addr);

            if (n <= 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) break; 
                break; 
            }

            consecutive_timeouts = 0; 

            if (buffer.type == GROUP_ACK) {
                received_acks.push_back(buffer);
            } 
            else if (buffer.type == NACK) {
                received_nacks.push_back(buffer);
            }
        } 

        for (const auto& buffer : received_acks) {
            if (groups_acked.find(buffer.group_id) == groups_acked.end()) {
                groups_acked[buffer.group_id] = true;
                cout << "[EC Sender] Received ACK for group " << buffer.group_id 
                     << " (" << groups_acked.size() << "/" << TOTAL_GROUPS << ")" << endl;
            }
        }

        for (const auto& buffer : received_nacks) {
            uint32_t gid = buffer.group_id;
            if (groups_acked.find(gid) == groups_acked.end()) { 
                cout << "[EC Sender] ❗️ Received NACK for group " << gid << ". Retransmitting..." << endl;
                
                for (int k = 0; k < EC_DATA_CHUNKS_K; ++k) {
                    if (buffer.payload[k] == 1) { // 1 means missing
                        ECPacket rtx_pkt;
                        generate_data_chunk(gid, k, rtx_pkt);
                        cout << "[EC Sender]   -> Retransmitting data chunk " << k << " for group " << gid << endl;
                        sock.send_packet(&rtx_pkt, sizeof(rtx_pkt));
                        retransmissions++;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }
        }
    }
    else if (activity == 0) {
        consecutive_timeouts++;
        cout << "[EC Sender] ...Timeout. Still waiting for " << (TOTAL_GROUPS - groups_acked.size()) << " ACKs..."
             << " (Inactivity: " << consecutive_timeouts << "/" << MAX_CONSECUTIVE_TIMEOUTS << ")" << endl;
        
        if (consecutive_timeouts >= MAX_CONSECUTIVE_TIMEOUTS) {
            cout << "[EC Sender] ❌ Max inactivity reached. Assuming transfer complated/failed." << endl;
            break; 
        }
    } else {
        perror("select error");
        break;
    }
  } // while

  auto end_total = chrono::high_resolution_clock::now();
  auto duration_total = chrono::duration_cast<chrono::milliseconds>(end_total - start).count();
  
  if (groups_acked.size() == TOTAL_GROUPS) {
      cout << "[EC Sender] ✅✅ All " << TOTAL_GROUPS << " groups ACKed." << endl;
  } else {
      cout << "[EC Sender] ❌ Final Timeout. Only " << groups_acked.size() << "/" << TOTAL_GROUPS << " groups were ACKed." << endl;
  }
  
  cout << "[EC Sender] Total retransmissions: " << retransmissions << endl;
  cout << "[EC Sender] Total time: " << duration_total << " ms." << endl;

  return 0;
}