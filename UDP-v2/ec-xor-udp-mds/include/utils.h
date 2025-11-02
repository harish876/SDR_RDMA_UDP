#pragma once
#include <cstdint>

// We'll use a 1KB data chunk size
constexpr std::size_t CHUNK_PAYLOAD_SIZE = 1024;

// --- UPDATED (k, m) PARAMETERS ---
// Use the (32, 8) "balanced" config from the paper (Figure 10d)
constexpr int EC_DATA_CHUNKS_K = 32;
constexpr int EC_PARITY_CHUNKS_M = 8;
constexpr int EC_GROUP_SIZE = (EC_DATA_CHUNKS_K + EC_PARITY_CHUNKS_M);

// Total chunks to send (1024 / 32 = 32 groups)
constexpr int TOTAL_DATA_CHUNKS = 1024;
constexpr int TOTAL_GROUPS = (TOTAL_DATA_CHUNKS / EC_DATA_CHUNKS_K);

enum PacketType : uint8_t {
  DATA_CHUNK = 0,
  PARITY_CHUNK = 1,
  NACK = 2,
  GROUP_ACK = 3
};

struct ECPacket {
  uint32_t  group_id;     // Which group (0 to TOTAL_GROUPS-1)
  PacketType type;          // Is this DATA or PARITY?
  uint8_t   chunk_index;  // Index within the group (0 to k-1 for data, 0 to m-1 for parity)
  uint32_t  data_size;    // How much data is in this payload
  
  // For NACK, payload[0...k-1] will be a bitmap: 1=missing, 0=received
  char      payload[CHUNK_PAYLOAD_SIZE]; 
};