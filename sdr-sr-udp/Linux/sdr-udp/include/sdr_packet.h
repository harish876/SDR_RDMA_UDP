#pragma once

#include <cstdint>
#include <cstring>
#include <arpa/inet.h>

namespace sdr {

// Packet types
enum class PacketType : uint8_t {
    DATA = 0,       // Data packet
    PARITY = 1,     // Parity packet (for future EC use)
    ACK = 2,        // Acknowledgment (for future SR use)
    NACK = 3,       // Negative acknowledgment (for future SR use)
    CTS = 4         // Clear to Send (not used in UDP packets, only TCP)
};

// Bitpacked UDP packet header
// Total header size: 16 bytes (128 bits)
// Layout:
//   magic:      16 bits
//   type:       8 bits
//   transfer_id: 32 bits
//   msg_id:     10 bits (part of 32-bit field)
//   packet_offset: 18 bits (part of 32-bit field)
//   submsg_id:  16 bits (for EC group index, future use)
//   chunk_seq:  32 bits (chunk sequence number)
//   packets_per_chunk: 16 bits
//   fec_k:      16 bits (for future EC use)
//   fec_m:      16 bits (for future EC use)
//   parity_idx: 16 bits (if type=PARITY, which parity chunk)
//   payload_len: 16 bits (actual payload size, useful for last packet)
//   flags:      8 bits (optional flags)
struct __attribute__((packed)) SDRPacketHeader {
    uint16_t magic;              // Magic number: 0x5344 ("SD")
    uint8_t type;                // PacketType enum value
    uint8_t _reserved1;          // Reserved for alignment
    
    uint32_t transfer_id;        // Transfer identifier
    
    // Pack msg_id (10 bits) and packet_offset (18 bits) into 32 bits
    uint32_t msg_id : 10;        // Message ID (0-1023)
    uint32_t packet_offset : 18; // Packet offset within message (supports up to 1GiB with 4KiB MTU)
    uint32_t _reserved2 : 4;     // Reserved bits
    
    uint16_t submsg_id;          // Submessage ID for EC groups (future use)
    
    uint32_t chunk_seq;          // Chunk sequence number
    uint16_t packets_per_chunk;  // Packets per chunk (P)
    uint16_t fec_k;              // FEC data chunks
    uint16_t fec_m;              // FEC parity chunks
    uint16_t parity_idx;         // Parity index (if type=PARITY)
    uint16_t payload_len;        // Actual payload length (can be < MTU for last packet)
    uint8_t flags;               // Optional flags
    uint8_t _reserved3[3];       // Padding to 16-byte boundary
    
    // Helper methods
    static constexpr uint16_t MAGIC_VALUE = 0x5344; // "SD"
    
    // Calculate chunk ID from packet offset
    uint32_t get_chunk_id() const {
        if (packets_per_chunk == 0) return 0;
        return packet_offset / packets_per_chunk;
    }
    
    // Calculate packet index within chunk
    uint32_t get_packet_in_chunk() const {
        if (packets_per_chunk == 0) return 0;
        return packet_offset % packets_per_chunk;
    }
    
    // Validate header
    bool is_valid() const {
        return magic == MAGIC_VALUE;
    }
    
    // Serialization (for network byte order conversion)
    // Note: Bitfields (msg_id, packet_offset) require special handling
    // For now, we assume host and network byte order are compatible for bitfields
    void to_network_order() {
        magic = htons(magic);
        transfer_id = htonl(transfer_id);
        chunk_seq = htonl(chunk_seq);
        submsg_id = htons(submsg_id);
        packets_per_chunk = htons(packets_per_chunk);
        fec_k = htons(fec_k);
        fec_m = htons(fec_m);
        parity_idx = htons(parity_idx);
        payload_len = htons(payload_len);
        // Note: msg_id and packet_offset are bitfields - handle carefully in production
    }
    
    void to_host_order() {
        magic = ntohs(magic);
        transfer_id = ntohl(transfer_id);
        chunk_seq = ntohl(chunk_seq);
        submsg_id = ntohs(submsg_id);
        packets_per_chunk = ntohs(packets_per_chunk);
        fec_k = ntohs(fec_k);
        fec_m = ntohs(fec_m);
        parity_idx = ntohs(parity_idx);
        payload_len = ntohs(payload_len);
    }
};

// Complete SDR packet structure (header + payload)
struct SDRPacket {
    SDRPacketHeader header;
    uint8_t payload[];  // Flexible array member (payload follows header)
    
    // Maximum payload size (assuming 1500 byte MTU - 8 byte UDP header - 16 byte SDR header)
    static constexpr size_t MAX_PAYLOAD_SIZE = 1500 - 8 - sizeof(SDRPacketHeader);
    
    // Get total packet size
    size_t get_total_size() const {
        return sizeof(SDRPacketHeader) + header.payload_len;
    }
    
    // Create a data packet
    static SDRPacket* create_data_packet(uint32_t transfer_id, uint32_t msg_id,
                                        uint32_t packet_offset, uint16_t packets_per_chunk,
                                        const uint8_t* data, size_t data_len) {
        if (data_len > MAX_PAYLOAD_SIZE) {
            return nullptr;
        }
        
        // Allocate packet with payload
        size_t total_size = sizeof(SDRPacketHeader) + data_len;
        SDRPacket* packet = reinterpret_cast<SDRPacket*>(new uint8_t[total_size]);
        
        // Initialize header
        std::memset(&packet->header, 0, sizeof(SDRPacketHeader));
        packet->header.magic = SDRPacketHeader::MAGIC_VALUE;
        packet->header.type = static_cast<uint8_t>(PacketType::DATA);
        packet->header.transfer_id = transfer_id;
        packet->header.msg_id = msg_id;
        packet->header.packet_offset = packet_offset;
        packet->header.packets_per_chunk = packets_per_chunk;
        packet->header.payload_len = static_cast<uint16_t>(data_len);
        
        // Copy payload
        std::memcpy(packet->payload, data, data_len);
        
        return packet;
    }
    
    // Destroy packet
    static void destroy(SDRPacket* packet) {
        if (packet) {
            delete[] reinterpret_cast<uint8_t*>(packet);
        }
    }
};

// Note: Using system htons/htonl/ntohs/ntohl from <arpa/inet.h>

} // namespace sdr

