#include "erasure_code.h"
#include <random>
#include <chrono>

namespace EC {

namespace Utils {

bool should_drop_packet(double loss_rate) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    
    return dis(gen) < loss_rate;
}

std::vector<uint8_t> generate_test_data(size_t size) {
    std::vector<uint8_t> data(size);
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 255);
    
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(dis(gen));
    }
    
    return data;
}

uint32_t simple_hash(const uint8_t* data, size_t size) {
    uint32_t hash = 0;
    for (size_t i = 0; i < size; ++i) {
        hash = hash * 31 + data[i];
    }
    return hash;
}

} // namespace Utils

} // namespace EC
