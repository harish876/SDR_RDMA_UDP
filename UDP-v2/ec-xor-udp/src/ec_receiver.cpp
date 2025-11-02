#include "udp_socket.h" // Corrected include
#include "utils.h"      // Corrected include
#include "xor_ec.h"     // Corrected include
#include <iostream>
#include <vector>
#include <map>
#include <cstring>

using namespace std;

// This struct will hold the state for each group we are receiving
struct GroupState {
  // We use a fixed-size vector. k data chunks + m parity chunks.
  PacketGroup packets;
  int received_count = 0;
  bool is_recovered = false;

  GroupState() : packets(EC_GROUP_SIZE) {
    // Initialize all packets as "missing"
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

  cout << "[EC Receiver] Listening on port " << listen_port << endl;
  cout << "[EC Receiver] Expecting " << TOTAL_GROUPS << " groups ("
       << EC_DATA_CHUNKS_K << " data, " << EC_PARITY_CHUNKS_M << " parity per group)." << endl;

  // A map to store the state of all incoming groups
  map<uint32_t, GroupState> groups;
  int groups_fully_recovered = 0;

  ECPacket buffer;
  sockaddr_in sender_addr{};

  while (groups_fully_recovered < TOTAL_GROUPS) {
    ssize_t n = sock.recv_bytes(&buffer, sizeof(buffer), sender_addr);
    if (n != sizeof(ECPacket)) {
      continue; // Bad packet
    }

    // 1. Packet is valid, check its group
    uint32_t gid = buffer.group_id;
    if (groups.find(gid) == groups.end()) {
      groups[gid] = GroupState(); // Create new state for this group
    }

    // If already recovered, skip
    if (groups[gid].is_recovered) {
      continue;
    }

    // 2. Store the packet in the correct slot
    int slot_index = -1;
    if (buffer.type == DATA_CHUNK && buffer.chunk_index < EC_DATA_CHUNKS_K) {
      slot_index = buffer.chunk_index;
    } else if (buffer.type == PARITY_CHUNK && buffer.chunk_index < EC_PARITY_CHUNKS_M) {
      slot_index = EC_DATA_CHUNKS_K + buffer.chunk_index;
    } else {
      continue; // Invalid index
    }

    if (groups[gid].packets[slot_index].data_size == 0) {
      // This is a new packet for this group
      groups[gid].packets[slot_index] = buffer;
      groups[gid].received_count++;
    }

    // 3. Try to decode if we have enough packets
    // We try to decode once we have 'k' packets, as that's the minimum for recovery
    if (groups[gid].received_count >= EC_DATA_CHUNKS_K) {
      if (XORErasureCoding::decode(groups[gid].packets)) {
        // Success!
        groups[gid].is_recovered = true;
        groups_fully_recovered++;
        cout << "[EC Receiver] ✅ Group " << gid << " successfully recovered! ("
             << groups_fully_recovered << "/" << TOTAL_GROUPS << ")" << endl;
      }
    }
  } // while

  cout << "[EC Receiver] ✅✅ All " << TOTAL_GROUPS << " groups recovered. Transfer complete." << endl;
  return 0;
}