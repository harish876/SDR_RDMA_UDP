#include "erasure_code.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace EC {

// Simple Reed-Solomon implementation using XOR-based parity
// For production, use a proper library like libfec or Intel ISA-L
class SimpleReedSolomon {
public:
    SimpleReedSolomon(int k, int m) : k_(k), m_(m) {}
    
    std::vector<std::vector<uint8_t>> encode(const std::vector<uint8_t>& data) const {
        std::vector<std::vector<uint8_t>> packets(k_ + m_);
        
        // Split data into k packets
        size_t packet_size = data.size() / k_;
        for (int i = 0; i < k_; ++i) {
            size_t start = i * packet_size;
            size_t end = (i == k_ - 1) ? data.size() : (i + 1) * packet_size;
            packets[i].assign(data.begin() + start, data.begin() + end);
        }
        
        // Generate parity packets using XOR
        for (int p = 0; p < m_; ++p) {
            packets[k_ + p].resize(packet_size, 0);
            
            // XOR all data packets
            for (int i = 0; i < k_; ++i) {
                for (size_t j = 0; j < packets[k_ + p].size() && j < packets[i].size(); ++j) {
                    packets[k_ + p][j] ^= packets[i][j];
                }
            }
        }
        
        return packets;
    }
    
    std::vector<uint8_t> decode(const std::vector<std::vector<uint8_t>>& packets, 
                               const std::vector<bool>& received) const {
        if (packets.size() != k_ + m_ || received.size() != k_ + m_) {
            return {};
        }
        
        // Count received packets
        int received_count = 0;
        for (bool r : received) {
            if (r) received_count++;
        }
        
        // Need at least k packets to decode
        if (received_count < k_) {
            return {};
        }
        
        // For this simple implementation, we'll just concatenate the first k received packets
        std::vector<uint8_t> result;
        int data_packets_found = 0;
        
        for (int i = 0; i < k_ && data_packets_found < k_; ++i) {
            if (received[i] && i < packets.size()) {
                result.insert(result.end(), packets[i].begin(), packets[i].end());
                data_packets_found++;
            }
        }
        
        return result;
    }
    
private:
    int k_, m_;
};

ReedSolomon::ReedSolomon(int k, int m) : k_(k), m_(m) {
    rs_encoder_ = new SimpleReedSolomon(k, m);
}

ReedSolomon::~ReedSolomon() {
    delete static_cast<SimpleReedSolomon*>(rs_encoder_);
}

std::vector<std::vector<uint8_t>> ReedSolomon::encode(const std::vector<uint8_t>& data) const {
    return static_cast<SimpleReedSolomon*>(rs_encoder_)->encode(data);
}

std::vector<uint8_t> ReedSolomon::decode(const std::vector<std::vector<uint8_t>>& packets, 
                                        const std::vector<bool>& received) const {
    return static_cast<SimpleReedSolomon*>(rs_encoder_)->decode(packets, received);
}

} // namespace EC
