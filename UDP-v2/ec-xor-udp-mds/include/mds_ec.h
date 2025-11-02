#pragma once
#include "utils.h"
#include <vector>

// Define a type for a list of packets
using PacketGroup = std::vector<ECPacket>;

namespace MDSErasureCoding {

  /**
   * @brief Initializes the Reed-Solomon encoding tables.
   * This MUST be called once at the start of the program.
   */
  void init();

  /**
   * @brief Encodes a group of 'k' data packets into 'm' parity packets
   * using Reed-Solomon.
   */
  void encode(const PacketGroup& data_packets, PacketGroup& parity_packets);

  /**
   * @brief Attempts to decode a group of received packets.
   * This is the powerful part. It can recover up to 'm' missing packets.
   */
  bool decode(PacketGroup& received_packets);

} // namespace MDSErasureCoding