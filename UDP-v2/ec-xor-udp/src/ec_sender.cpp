#include "udp_socket.h" // Corrected include
#include "utils.h"      // Corrected include
#include "xor_ec.h"     // Corrected include
#include <iostream>
#include <vector>
#include <string>
#include <cstring> // For memset
#include <chrono>
#include <thread>

using namespace std;

int main(int argc, char* argv[]) {
  if (argc != 3) {
    cerr << "Usage: " << argv[0] << " <receiver_ip> <receiver_port>\n";
    return 1;
  }

  string receiver_ip = argv[1];
  int receiver_port = stoi(argv[2]);

  UDPSocket sock;
  sock.set_peer(receiver_ip, receiver_port);

  cout << "[EC Sender] Starting to send " << TOTAL_GROUPS << " groups ("
       << TOTAL_DATA_CHUNKS << " data chunks) to "
       << receiver_ip << ":" << receiver_port << endl;

  // Create some dummy data
  vector<char> file_data(TOTAL_DATA_CHUNKS * CHUNK_PAYLOAD_SIZE);
  for (size_t i = 0; i < file_data.size(); ++i) {
    file_data[i] = (i % 26) + 'A';
  }

  auto start = chrono::high_resolution_clock::now();

  for (int g = 0; g < TOTAL_GROUPS; ++g) {
    // 1. Prepare 'k' data packets for this group
    PacketGroup data_to_encode(EC_DATA_CHUNKS_K);
    for (int k = 0; k < EC_DATA_CHUNKS_K; ++k) {
      data_to_encode[k].group_id = g;
      data_to_encode[k].type = DATA_CHUNK;
      data_to_encode[k].chunk_index = k;
      data_to_encode[k].data_size = CHUNK_PAYLOAD_SIZE;
      
      // Copy data from our "file"
      size_t offset = (g * EC_DATA_CHUNKS_K + k) * CHUNK_PAYLOAD_SIZE;
      memcpy(data_to_encode[k].payload, &file_data[offset], CHUNK_PAYLOAD_SIZE);
    }

    // 2. Encode to get 'm' parity packets
    PacketGroup parity_packets = XORErasureCoding::encode(data_to_encode);

    // 3. Send all 'k' data packets
    for (const auto& pkt : data_to_encode) {
      sock.send_packet(&pkt, sizeof(ECPacket));
      std::this_thread::sleep_for(std::chrono::microseconds(10)); // Pace it slightly
    }

    // 4. Send all 'm' parity packets
    for (const auto& pkt : parity_packets) {
      sock.send_packet(&pkt, sizeof(ECPacket));
      std::this_thread::sleep_for(std::chrono::microseconds(10)); // Pace it slightly
    }

    if ((g + 1) % 10 == 0) {
      cout << "[EC Sender] Sent group " << g + 1 << "/" << TOTAL_GROUPS << endl;
    }
  }

  auto end = chrono::high_resolution_clock::now();
  auto duration = chrono::duration_cast<chrono::milliseconds>(end - start).count();
  cout << "[EC Sender] ✅ All " << TOTAL_GROUPS << " groups sent (" << duration << " ms)." << endl;

  return 0;
}