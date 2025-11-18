#include <iostream>
#include <cstdint>
#include <cstring>

struct MessageContext {
    uint32_t generation;        // Current generation of the message
    enum class MessageState { ACTIVE, COMPLETED, NULL_STATE } state;
};

struct SDRPacketHeader {
    uint32_t msg_id;
    uint32_t transfer_id;       // Generation field in header
};

// Mock of process_packet function (simplified)
void process_packet(MessageContext* msg_ctx, const SDRPacketHeader& header) {
    if (!msg_ctx) return;

    // Check generation
    if (msg_ctx->generation != header.transfer_id) {
        std::cout << "[Discarded] Packet msg_id=" << header.msg_id
                  << " with generation=" << header.transfer_id
                  << " (current=" << msg_ctx->generation << ")\n";
        return;
    }

    std::cout << "[Accepted] Packet msg_id=" << header.msg_id
              << " with generation=" << header.transfer_id << "\n";
}

int main() {
    MessageContext msg_ctx;
    msg_ctx.generation = 1;
    msg_ctx.state = MessageContext::MessageState::ACTIVE;

    // Packet from the same generation -> accepted
    SDRPacketHeader pkt1;
    pkt1.msg_id = 0;
    pkt1.transfer_id = 1;
    process_packet(&msg_ctx, pkt1);

    // Packet from old generation -> discarded
    SDRPacketHeader pkt2;
    pkt2.msg_id = 0;
    pkt2.transfer_id = 0;
    process_packet(&msg_ctx, pkt2);

    // Packet from future generation -> discarded (simulated)
    SDRPacketHeader pkt3;
    pkt3.msg_id = 0;
    pkt3.transfer_id = 2;
    process_packet(&msg_ctx, pkt3);

    return 0;
}
