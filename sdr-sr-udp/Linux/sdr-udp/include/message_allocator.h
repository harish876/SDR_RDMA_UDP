#pragma once
#include <cstdint>
#include <array>
#include <mutex>

constexpr uint32_t MAX_MSG_ID = 1024; // 10-bit IDs

struct MessageSlot {
    uint32_t generation = 0; // generation of this slot
    bool in_use = false;
};

class MessageIDAllocator {
public:
    MessageIDAllocator() : current_id(0), current_generation(1) {}

    // Allocate next message ID and returns its generation
    uint32_t allocate(uint32_t &out_generation) {
        std::lock_guard<std::mutex> lock(mtx);
        uint32_t start_id = current_id;

        do {
            MessageSlot &slot = slots[current_id];
            if (!slot.in_use) {
                slot.in_use = true;
                slot.generation = current_generation;
                out_generation = current_generation;
                uint32_t allocated_id = current_id;
                current_id = (current_id + 1) % MAX_MSG_ID;
                // Increment generation if wrapped around
                if (current_id == 0) current_generation++;
                return allocated_id;
            }

            current_id = (current_id + 1) % MAX_MSG_ID;
            if (current_id == start_id) {
                // All slots in use
                out_generation = 0;
                return UINT32_MAX; 
            }
        } while (true);
    }

    // Free a slot after transfer complete
    void free(uint32_t msg_id) {
        if (msg_id >= MAX_MSG_ID) return;
        std::lock_guard<std::mutex> lock(mtx);
        slots[msg_id].in_use = false;
    }

        // New method
    void increment_generation(uint32_t msg_id) {
        if (msg_id >= MAX_MSG_ID) return;
        slots[msg_id].generation++;
    }

    // Get the generation of a message ID
    uint32_t get_generation(uint32_t msg_id) {
        if (msg_id >= MAX_MSG_ID) return 0;
        std::lock_guard<std::mutex> lock(mtx);
        return slots[msg_id].generation;
    }

private:
    std::array<MessageSlot, MAX_MSG_ID> slots;
    uint32_t current_id;
    uint32_t current_generation;
    std::mutex mtx;
};
