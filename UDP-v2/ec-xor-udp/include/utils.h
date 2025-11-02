#pragma once
#include <cstdint>

// We'll use a 1KB data chunk size
constexpr std::size_t CHUNK_PAYLOAD_SIZE = 1024;

// Define the (k, m) parameters for our XOR code
// We'll send 8 data chunks + 2 parity chunks per group
constexpr int EC_DATA_CHUNKS_K = 8;
constexpr int EC_PARITY_CHUNKS_M = 2;
constexpr int EC_GROUP_SIZE = (EC_DATA_CHUNKS_K + EC_PARITY_CHUNKS_M);

// Total chunks to send (must be a multiple of k)
constexpr int TOTAL_DATA_CHUNKS = 1024;
constexpr int TOTAL_GROUPS = (TOTAL_DATA_CHUNKS / EC_DATA_CHUNKS_K);

enum PacketType : uint8_t {
  DATA_CHUNK = 0,
  PARITY_CHUNK = 1
};

struct ECPacket {
  uint32_t  group_id;     // Which group (0 to TOTAL_GROUPS-1)
  PacketType type;          // Is this DATA or PARITY?
  uint8_t   chunk_index;  // Index within the group (0 to k-1 for data, 0 to m-1 for parity)
  uint32_t  data_size;    // How much data is in this payload
  char      payload[CHUNK_PAYLOAD_SIZE];
};