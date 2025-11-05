# SDR-RDMA Implementation Plan

## Overview

Implement SDR-RDMA in `external/sdr-rdma/` directory focusing on:

- Memory table creation and management
- Backend CQ poller for processing write_with_imm completions
- Frontend poller for chunk completion tracking
- Examples similar to sdr_udp structure (receiver/sender)

## Directory Structure

```
external/sdr-rdma/
├── include/
│   ├── sdr_rdma_memory.h      - Memory table management
│   ├── sdr_rdma_backend.h     - Backend CQ poller
│   └── sdr_rdma_frontend.h    - Frontend poller (or reuse from sdr-udp)
├── src/
│   ├── sdr_rdma_memory.cpp
│   └── sdr_rdma_backend.cpp
├── examples/
│   ├── sdr_rdma_receiver.cpp  - Receiver example
│   └── sdr_rdma_sender.cpp    - Sender example
└── CMakeLists.txt
```

## Core Components

### 1. Memory Table (`sdr_rdma_memory.h/cpp`)

- Allocate root buffer: `max_message_size * MAX_MESSAGES` (1024 messages)
- Register root MR with rdmapp::pd
- Message slot table: `msg_id -> {buffer_offset, local_mr, remote_mr}`
- Functions:
  - `allocate_slot(msg_id, buffer_size)` - Allocate message slot
  - `get_slot(msg_id)` - Get slot info
  - `update_slot_remote_mr(msg_id, remote_mr)` - Update remote MR for slot
  - `get_root_mr()` - Get root MR for exchange

### 2. Backend CQ Poller (`sdr_rdma_backend.h/cpp`)

- CQ polling coroutine task: `rdmapp::task<void> poll_cq(std::shared_ptr<rdmapp::cq> cq, MemoryTable* table)`
- Process write_with_imm completions from CQ
- Decode immediate data:
  ```cpp
  uint32_t msg_id = (imm >> 22) & 0x3FF;        // 10 bits
  uint32_t packet_offset = (imm >> 4) & 0x3FFFF; // 18 bits
  uint32_t user_imm = imm & 0xF;               // 4 bits
  ```

- Get message slot from memory table
- Update BackendBitmap (reuse from sdr-udp or copy)
- Run as detached coroutine task

### 3. Frontend Poller

- Reuse FrontendBitmap from sdr-udp (or copy to external/)
- Polls BackendBitmap and updates chunk completion
- Thread-based polling (existing implementation)

### 4. Examples

**Receiver (`sdr_rdma_receiver.cpp`):**

- Use rdmapp::acceptor for connection setup
- Establish QP, exchange root MR keys via QP send/recv
- Allocate root buffer and register root MR
- Post receive buffer, allocate message slot
- Start backend CQ poller task (detached)
- Start frontend poller thread
- Poll for completion

**Sender (`sdr_rdma_sender.cpp`):**

- Use rdmapp::connector for connection setup
- Receive root MR key from receiver
- Receive message slot info (msg_id + buffer MR) from receiver
- Send data: per-packet `qp->write_with_imm()`
- Encode immediate: `imm = (msg_id << 22) | (packet_offset << 4) | user_imm`
- Create remote_mr for each packet offset

## Implementation Steps

### Step 1: Memory Table Implementation

- Create `sdr_rdma_memory.h/cpp`
- Allocate root buffer (e.g., 1GB for 1024 messages of 1MB each)
- Register with pd->reg_mr()
- Maintain slot table with mutex protection
- Implement slot allocation/retrieval functions

### Step 2: Backend CQ Poller

- Create `sdr_rdma_backend.h/cpp`
- Implement CQ polling coroutine using rdmapp::cq_poller or direct polling
- Process completions, decode immediate data
- Update BackendBitmap (need to link or copy from sdr-udp)

### Step 3: Frontend Poller

- Copy or link FrontendBitmap from sdr-udp
- Or create minimal version in external/

### Step 4: Receiver Example

- Follow chunked_transmission.cc pattern
- Use acceptor to accept QP
- Exchange root MR keys
- Allocate message slot
- Start pollers
- Wait for completion

### Step 5: Sender Example

- Follow chunked_transmission.cc pattern
- Use connector to connect QP
- Receive root MR key
- Receive message slot info
- Send per-packet write_with_imm

### Step 6: Build Configuration

- Create CMakeLists.txt
- Link against rdmapp library
- Link or copy bitmap classes from sdr-udp

## Immediate Data Encoding

```cpp
// Encoding (sender)
uint32_t imm = (msg_id << 22) | (packet_offset << 4) | user_imm;

// Decoding (receiver)  
uint32_t msg_id = (imm >> 22) & 0x3FF;           // bits 22-31
uint32_t packet_offset = (imm >> 4) & 0x3FFFF;  // bits 4-21
uint32_t user_imm = imm & 0xF;                   // bits 0-3
```

## Protocol Flow

1. **Connection**: Receiver uses acceptor, sender uses connector
2. **Root MR Exchange**: After QP establishment, exchange root MR keys via QP send/recv
3. **Post-Receive**: Receiver allocates message slot, sends slot info + buffer MR to sender
4. **CTS**: Optional - can use QP send/recv or skip for simplicity
5. **Data Transfer**: 

   - Sender: per-packet write_with_imm
   - Receiver: CQ poller processes completions, updates BackendBitmap

6. **Frontend Polling**: FrontendBitmap polls BackendBitmap for chunk completion

## Dependencies

- rdmapp library (already in external/rdmapp)
- sdr-udp bitmap classes: BackendBitmap and FrontendBitmap (can link/include from sdr-udp/include)
- sdr-udp ConnectionContext and MessageContext (for message table - can link/include)
- C++17 coroutines (provided by rdmapp)

## Integration Notes

- Memory table works alongside ConnectionContext (doesn't replace it)
- BackendBitmap and FrontendBitmap from sdr-udp can be reused directly
- MessageContext already has the structure we need (msg_id, buffer, bitmaps)
- Just need to add MR references (either extend MessageContext or maintain separate memory table)
- Backend poller integrates: ConnectionContext->get_message(msg_id)->backend_bitmap->set_packet_received()