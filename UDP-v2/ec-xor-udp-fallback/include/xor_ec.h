#pragma once
#include "utils.h" // <-- Corrected include path
#include <vector>

// Define a type for a list of packets
using PacketGroup = std::vector<ECPacket>;

namespace XORErasureCoding {

  /**
   * @brief Encodes a group of 'k' data packets into 'm' parity packets.
   */
  PacketGroup encode(const PacketGroup& data_packets);

  /**
   * @brief Attempts to decode a group of received packets.
   */
  bool decode(PacketGroup& received_packets);

} // namespace XORErasureCoding