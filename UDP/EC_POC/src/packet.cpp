#include "erasure_code.h"
#include <cstring>
#include <algorithm>
#include <iostream>

namespace EC {

Packet::Packet(uint32_t seq, PacketType type, const std::vector<uint8_t>& payload) {
    header.magic = PacketHeader::MAGIC_NUMBER;
    header.version = PacketHeader::PROTOCOL_VERSION;
    header.flags = 0;
    header.sequence = seq;
    header.type = type;
    header.checksum = 0;  // Will be calculated after data is set
    
    data = payload;
    header.checksum = calculate_checksum();
}

std::vector<uint8_t> Packet::serialize() const {
    std::vector<uint8_t> result;
    result.reserve(sizeof(PacketHeader) + data.size());
    
    // Serialize header
    const uint8_t* header_ptr = reinterpret_cast<const uint8_t*>(&header);
    result.insert(result.end(), header_ptr, header_ptr + sizeof(PacketHeader));
    
    // Serialize data
    result.insert(result.end(), data.begin(), data.end());
    
    return result;
}

std::unique_ptr<Packet> Packet::deserialize(const uint8_t* data, size_t size) {
    if (size < sizeof(PacketHeader)) {
        return nullptr;
    }
    
    auto packet = std::make_unique<Packet>();
    
    // Deserialize header
    std::memcpy(&packet->header, data, sizeof(PacketHeader));
    
    // Validate magic number
    if (packet->header.magic != PacketHeader::MAGIC_NUMBER) {
        return nullptr;
    }
    
    // Deserialize data
    size_t data_size = size - sizeof(PacketHeader);
    packet->data.resize(data_size);
    std::memcpy(packet->data.data(), data + sizeof(PacketHeader), data_size);
    
    // Validate checksum
    if (!packet->is_valid()) {
        return nullptr;
    }
    
    return packet;
}

uint32_t Packet::calculate_checksum() const {
    uint32_t checksum = 0;
    
    // Calculate checksum over header (excluding checksum field)
    const uint8_t* header_ptr = reinterpret_cast<const uint8_t*>(&header);
    for (size_t i = 0; i < sizeof(PacketHeader) - sizeof(uint32_t); ++i) {
        checksum += header_ptr[i];
    }
    
    // Calculate checksum over data
    for (uint8_t byte : data) {
        checksum += byte;
    }
    
    return checksum;
}

bool Packet::is_valid() const {
    // Check magic number
    if (header.magic != PacketHeader::MAGIC_NUMBER) {
        return false;
    }
    
    // Check version
    if (header.version != PacketHeader::PROTOCOL_VERSION) {
        return false;
    }
    
    // Check checksum
    uint32_t calculated_checksum = calculate_checksum();
    if (header.checksum != calculated_checksum) {
        return false;
    }
    
    return true;
}

} // namespace EC
