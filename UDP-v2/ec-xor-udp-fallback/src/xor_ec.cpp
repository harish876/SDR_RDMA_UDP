#include "xor_ec.h"
#include "utils.h"
#include <cstring> // For memset

namespace XORErasureCoding {

/**
 * @brief Helper function to XOR the payload of one packet into another.
 */
void xor_payloads(ECPacket& dest, const ECPacket& src) {
  for (size_t i = 0; i < CHUNK_PAYLOAD_SIZE; ++i) {
    dest.payload[i] ^= src.payload[i];
  }
}

PacketGroup encode(const PacketGroup& data_packets) {
  // Create 'm' empty parity packets
  PacketGroup parity_packets(EC_PARITY_CHUNKS_M);
  for (int i = 0; i < EC_PARITY_CHUNKS_M; ++i) {
    parity_packets[i].type = PARITY_CHUNK;
    parity_packets[i].chunk_index = i;
    parity_packets[i].group_id = data_packets[0].group_id; // Assume all data packets have same group_id
    parity_packets[i].data_size = CHUNK_PAYLOAD_SIZE; // Will be full
    std::memset(parity_packets[i].payload, 0, CHUNK_PAYLOAD_SIZE);
  }

  // As per, XOR data chunks into the correct parity chunk
  for (int k_idx = 0; k_idx < EC_DATA_CHUNKS_K; ++k_idx) {
    int m_idx = k_idx % EC_PARITY_CHUNKS_M;
    xor_payloads(parity_packets[m_idx], data_packets[k_idx]);
  }

  return parity_packets;
}

bool decode(PacketGroup& received_packets) {
  // 1. Check which data packets are missing for each modulo group
  // We need two arrays: one to count missing, one to store the index of *what* is missing
  int missing_count[EC_PARITY_CHUNKS_M] = {0};
  int missing_index[EC_PARITY_CHUNKS_M] = {-1}; // Stores the 'k' index (0..k-1)

  // --- THIS IS THE FIXED LOOP ---
  for (int k_idx = 0; k_idx < EC_DATA_CHUNKS_K; ++k_idx) {
    if (received_packets[k_idx].data_size == 0) { // Assuming data_size 0 means missing
      int m_idx = k_idx % EC_PARITY_CHUNKS_M;
      missing_count[m_idx]++;
      missing_index[m_idx] = k_idx; // Store the index of the missing packet (0..k-1)
    }
  }

  // 2. Try to recover
  for (int m_idx = 0; m_idx < EC_PARITY_CHUNKS_M; ++m_idx) {
    if (missing_count[m_idx] == 0) {
      // This group is healthy. Nothing to do.
      continue;
    }

    if (missing_count[m_idx] == 1) {
      // Exactly one packet lost. We can recover it!
      // We need the corresponding parity packet for this group.
      int parity_packet_idx = EC_DATA_CHUNKS_K + m_idx;
      if (received_packets[parity_packet_idx].data_size == 0) {
        // We lost the data AND the parity chunk. Cannot recover.
        continue; // Move to next m_idx
      }

      // Start with the parity chunk
      int missing_k_idx = missing_index[m_idx];
      ECPacket& recovery_packet = received_packets[missing_k_idx];
      recovery_packet = received_packets[parity_packet_idx]; // Copy parity data
      
      // Fix metadata
      recovery_packet.type = DATA_CHUNK;
      recovery_packet.chunk_index = missing_k_idx;
      recovery_packet.data_size = CHUNK_PAYLOAD_SIZE; // It's now "recovered"

      // Now, XOR all *other* data packets from this group
      for (int k_idx = 0; k_idx < EC_DATA_CHUNKS_K; ++k_idx) {
        if (k_idx % EC_PARITY_CHUNKS_M == m_idx && k_idx != missing_k_idx) {
          if(received_packets[k_idx].data_size > 0) { // Ensure we don't xor empty packets
             xor_payloads(recovery_packet, received_packets[k_idx]);
          }
        }
      }
    } 
    // If missing_count[m_idx] > 1, we can't recover this group, so we just continue
  }

  // 3. Final check: are all 'k' data packets present now?
  for (int k_idx = 0; k_idx < EC_DATA_CHUNKS_K; ++k_idx) {
    if (received_packets[k_idx].data_size == 0) {
      return false; // Recovery failed for at least one data chunk
    }
  }

  return true; // All data packets are present
}

} // namespace XORErasureCoding