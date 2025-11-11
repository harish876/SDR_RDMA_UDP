# SDR-RDMA: Software-Defined Reliability for Cross-WAN RDMA Communication

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![C++20](https://img.shields.io/badge/C++-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

**SDR-RDMA** (Software-Defined Reliability for RDMA) is a research project that enables reliable, high-performance RDMA communication across wide-area networks (WAN) using Forward Error Correction (FEC) and other adaptive reliability schemes.

## RDMA Implementation Overview

This repository contains **two complementary implementations** of the SDR-RDMA architecture:

### 1. RDMA Implementation (RoCEv2)

The **primary RDMA implementation** uses **RoCEv2** (RDMA over Converged Ethernet v2) to establish cross-WAN RDMA connections. This implementation leverages the `rdmapp` library (included in `external/rdmapp/`) to provide a modern C++20 coroutine-based interface for RDMA operations.

**Key Features:**
- **Cross-WAN RDMA Connection**: Establishes reliable RDMA connections over long-haul networks using RoCEv2
- **Chunked Data Transfer**: Implements a chunked transmission scheme that splits data into MTU-sized units (typically 1024-4096 bytes) for efficient transfer
- **Reed-Solomon FEC**: Integrates Forward Error Correction using Reed-Solomon erasure coding to recover from packet losses without retransmission
- **High Bandwidth Utilization**: Designed to fully utilize **20 Gbits/s bandwidth** over cross-WAN links
- **Zero-Copy Operations**: Leverages RDMA's zero-copy semantics for optimal performance

**Implementation Details:**
- Uses `rdmapp` library for queue pair (QP) management and RDMA operations
- Chunks large messages into MTU-aligned packets (configurable packet size, default 1024 bytes)
- Each chunk is sent as an individual RDMA packet with metadata (sequence number, type, remote address, rkey)
- Implements bitmap-based tracking of received chunks (similar to UDP prototype)
- Supports both data and parity packet transmission for FEC
- Uses RDMA Read operations to fetch packet data from remote memory regions

**Example Usage:**
```cpp
// RDMA sender with FEC
RDMA_EC::RDMASender sender(acceptor, config);
co_await sender.send_data(large_data_buffer);

// RDMA receiver with FEC decoding
RDMA_EC::RDMAReceiver receiver(connector, config);
auto received_data = co_await receiver.receive_data();
```

### 2. UDP Prototype Implementation

The **UDP-based prototype** (in `sdr-udp/`) serves as a reference implementation and testing framework that demonstrates the SDR bitmap concept without requiring RDMA hardware. This prototype uses:
- **TCP** for control plane (connection establishment, CTS messages)
- **UDP** for data plane (high-speed packet transfer)
- **Bitmap tracking** for chunk-level progress monitoring

The UDP prototype is useful for:
- Development and testing of reliability algorithms
- Understanding the SDR bitmap API
- Prototyping new reliability schemes before RDMA integration
- Testing in environments without RDMA hardware

---

**Goal**: Achieve **20 Gbits/s bandwidth utilization** over cross-WAN links by combining:
1. Efficient chunked data transfer aligned to MTU boundaries
2. Forward Error Correction to avoid retransmission overhead
3. RDMA's zero-copy, kernel-bypass capabilities
4. Software-defined reliability layer for adaptive error recovery

## Overview

RDMA (Remote Direct Memory Access) is essential for efficient distributed training across datacenters, but millisecond-scale latencies in long-haul links complicate reliability design. Traditional Selective Repeat (SR) algorithms can be inefficient over WAN links with varying drop rates, distances, and bandwidth characteristics. SDR-RDMA provides a **software-defined reliability stack** that enables alternative reliability schemes (like Erasure Coding) on existing RDMA hardware.

### Key Innovation: Partial Message Completion

SDR-RDMA introduces a novel **partial message completion** API that extends standard RDMA semantics with a **receive buffer bitmap**. This bitmap tracks which chunks of a message have been received, allowing applications to implement custom reliability schemes tailored to specific network conditions while preserving zero-copy RDMA benefits.

## Problem Statement

### Challenges of Cross-WAN RDMA

1. **Variable Packet Loss**: WAN links exhibit highly variable packet drop rates (10⁻⁴ to 10⁻¹) depending on payload size, network conditions, and distance
2. **High Latency**: Millisecond-scale RTTs make traditional ACK-based reliability inefficient
3. **Hardware Limitations**: Existing RDMA NICs implement reliability in ASIC, making it impossible to deploy alternative algorithms without waiting for next-generation silicon
4. **Coarse Completion Semantics**: Standard unreliable RDMA Write drops entire messages if even one packet is lost, forcing full retransmission

### Why FEC?

Forward Error Correction (FEC) with Erasure Coding allows receivers to recover lost data chunks using parity chunks, avoiding costly retransmissions over high-latency links. SDR-RDMA's bitmap API enables efficient FEC implementation by tracking which chunks are missing.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                          │
│         (Reliability Algorithms: SR, EC, Hybrid)             │
└─────────────────────────────────────────────────────────────┘
                            ↕
┌─────────────────────────────────────────────────────────────┐
│                    SDR Middleware SDK                         │
│  • Partial Message Completion API                            │
│  • Chunk Bitmap Tracking                                      │
│  • Progress Engine                                            │
└─────────────────────────────────────────────────────────────┘
                            ↕
┌─────────────────────────────────────────────────────────────┐
│              RDMA Transport (UC/UD)                          │
│  • Unreliable RDMA Write                                      │
│  • Zero-copy data movement                                   │
└─────────────────────────────────────────────────────────────┘
```

### Core Components

1. **BackendBitmap**: Tracks individual packet reception using lock-free atomic operations
2. **FrontendBitmap**: Polls backend bitmap and maintains chunk-level completion status
3. **UDPReceiver**: Handles incoming UDP packets and updates backend bitmap (UDP prototype)
4. **TCPControl**: Manages TCP connection for control messages (CTS/connection setup)
5. **ConnectionContext**: Manages per-connection state and message tracking

## Features

- ✅ **Partial Message Completion**: Bitmap-based tracking of received chunks
- ✅ **Lock-Free Operations**: High-performance atomic bitmap updates
- ✅ **Chunk-Based Delivery**: Transfers data in chunks for granular progress tracking
- ✅ **FEC-Ready**: Bitmap API enables efficient Erasure Coding implementation
- ✅ **Zero-Copy**: Preserves RDMA zero-copy semantics (in hardware implementation)
- ✅ **Adaptive Reliability**: Supports SR, EC, and hybrid schemes
- ✅ **Cross-WAN Optimized**: Designed for high-latency, lossy network conditions

## Current Implementation

This repository contains a **UDP-based prototype** that demonstrates the SDR bitmap concept. The prototype uses:
- **TCP** for control plane (connection establishment, CTS messages)
- **UDP** for data plane (high-speed packet transfer)
- **Bitmap tracking** for chunk-level progress monitoring

> **Note**: The production SDR-RDMA implementation offloads to NVIDIA's Data Path Accelerator (DPA) for line-rate performance. This UDP prototype serves as a reference implementation and testing framework.

## Building

### Prerequisites

**For UDP Prototype:**
- CMake 3.10 or higher
- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- pthread library (usually included on Unix systems)

**For RDMA Implementation:**
- C++20 compatible compiler (GCC 10+, Clang 10+)
- `libibverbs` development headers (`libibverbs-dev` on Ubuntu/Debian)
- RDMA-capable NIC with RoCEv2 support
- Properly configured RoCEv2 network (for cross-WAN, requires appropriate routing)

### Build Steps

#### Building UDP Prototype

```bash
# Clone the repository
git clone <repository-url>
cd SDR_RDMA_UDP

# Create build directory
cd sdr-udp
mkdir build
cd build

# Configure with CMake
cmake ..

# Build the project
make

# This creates:
# - libsdr_udp.a (static library)
# - sdr_test_receiver (receiver executable)
# - sdr_test_sender (sender executable)
```

#### Building RDMA Implementation

```bash
# Navigate to rdmapp directory
cd external/rdmapp

# Create build directory
mkdir build
cd build

# Configure with CMake
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..

# Build the project
cmake --build .

# This builds:
# - RDMA++ library
# - RDMA examples including rdma_reed_solomon
# - Bandwidth testing tools (send_bw, write_bw)
```

**Note**: The RDMA implementation requires RDMA-capable hardware and proper network configuration. For cross-WAN testing, ensure RoCEv2 is properly configured on both endpoints.

## Usage

### Quick Start

**Terminal 1 - Start Receiver:**
```bash
cd build
./sdr_test_receiver <tcp_port> <udp_port> [message_size] <config_file>
```

Example:
```bash
./sdr_test_receiver 8888 9999 10485760 ../config/receiver.config
```

**Terminal 2 - Start Sender:**
```bash
cd build
./sdr_test_sender <server_ip> <tcp_port> <udp_port> [message_size]
```

Example:
```bash
./sdr_test_sender 127.0.0.1 8888 9999 10485760
```

### Configuration

The receiver accepts a configuration file with the following parameters:

```ini
# Maximum Transmission Unit (MTU) - packet payload size in bytes
mtu_bytes=128

# Number of packets per chunk
packets_per_chunk=32

# Transfer ID (optional, defaults to 1)
transfer_id=1
```

### Progress Visualization

The receiver displays real-time progress including:
- Overall message completion percentage
- Per-chunk packet progress (cycling through chunk windows)
- Visual progress bars for each chunk

Example output:
```
[Receiver] Message Progress: [==================================================] 64.0% (8192/12800 chunks)
Showing chunks 4155-4169 (of 12800):
  Chunk 4155: [##############################] 100.0% ( 32/32 packets) ✓
  Chunk 4156: [##############################] 100.0% ( 32/32 packets) ✓
  ...
```

## API Overview

### Context Management

```cpp
// Create SDR context
SDRContext* ctx = sdr_ctx_create("device_name");

// Destroy context
sdr_ctx_destroy(ctx);
```

### Connection Establishment

**Receiver Side:**
```cpp
// Listen for incoming connections
SDRConnection* conn = sdr_listen(ctx, tcp_port);
conn->tcp_server->accept_connection();
```

**Sender Side:**
```cpp
// Connect to receiver
SDRConnection* conn = sdr_connect(ctx, server_ip, tcp_port);
```

### Receive Operations

```cpp
// Post a receive buffer
void* buffer = malloc(message_size);
SDRRecvHandle* recv_handle = nullptr;
sdr_recv_post(conn, buffer, message_size, &recv_handle);

// Poll chunk bitmap for progress
const uint8_t* bitmap = nullptr;
size_t bitmap_len = 0;
sdr_recv_bitmap_get(recv_handle, &bitmap, &bitmap_len);

// Get completion status
uint32_t chunks_completed = recv_handle->msg_ctx->frontend_bitmap->get_total_chunks_completed();
uint32_t total_chunks = recv_handle->msg_ctx->total_chunks;

// Complete receive (sends ACK/NACK to sender)
sdr_recv_complete(recv_handle);
```

### Send Operations

```cpp
// Send data
const void* buffer = data;
size_t length = data_size;
SDRSendHandle* send_handle = nullptr;
sdr_send_post(conn, buffer, length, &send_handle);

// Poll for completion (waits for receiver ACK)
sdr_send_poll(send_handle);
```

## Reliability Schemes

SDR-RDMA's bitmap API enables multiple reliability strategies:

### 1. Selective Repeat (SR)
- Receiver polls bitmap and sends ACKs with missing chunk information
- Sender retransmits only missing chunks
- Efficient for low drop rates and short messages

### 2. Erasure Coding (EC)
- Sender encodes data chunks with parity chunks
- Receiver uses bitmap to identify missing chunks
- Decodes using received data + parity chunks
- Avoids retransmissions, ideal for high drop rates

### 3. Hybrid Schemes
- Adaptive switching between SR and EC based on network conditions
- Uses bitmap to make informed decisions about recovery strategy

## Research Context

This project is based on research from:

- **ETH Zurich** (Mikhail Khalilov, Siyuan Shen, Marcin Chrapek, Tiancheng Chen, Kenji Nakano, Torsten Hoefler)
- **NVIDIA** (Peter-Jan Gootzen, Salvatore Di Girolamo, Rami Nudelman, Gil Bloch)
- **Microsoft** (Sreevatsa Anantharamu, Mahmoud Elhaddad, Jithin Jose, Abdul Kabbani, Scott Moe, Konstantin Taranov, Zhuolong Yu, Jie Zhang)
- **Swiss National Supercomputing Centre (CSCS)** (Nicola Mazzoletti)

### Key Publications

The full implementation guide and research details are available in `docs/IMPLEMENTATION_GUIDE.md`.

**Key Contributions:**
1. Analysis of inter-datacenter communication challenges
2. SDR-RDMA architecture decoupling reliability logic from packet processing
3. Data path offloading for line-rate performance on commodity NICs
4. Framework for simulating and analyzing SDR-based reliability algorithms

## Performance

- **Throughput**: Designed to sustain packet rates on links up to 3.2 Tbit/s (with DPA offloading)
- **Latency**: Minimal overhead on CPU and system memory bandwidth
- **Reliability**: Up to 5× improvement in average completion time, 12× at 99.9th percentile

## Limitations

### Current UDP Prototype

- **Packet Offset Limit**: Currently limited by 18-bit packet offset in packet header (~32 MiB with 128-byte MTU)
- **No Hardware Offloading**: CPU-based processing (production version uses DPA)
- **UDP Transport**: Uses UDP instead of RDMA (for prototyping/testing)

### Future Work

- Extend packet offset to support larger messages
- Implement full RDMA transport integration
- Add FEC encoding/decoding library integration
- Support for adaptive reliability scheme selection

## Troubleshooting

### Port Already in Use
```bash
lsof -ti:8888 | xargs kill -9  # TCP port
lsof -ti:9999 | xargs kill -9  # UDP port
```

### Connection Timeout
- Ensure firewall allows TCP and UDP traffic on specified ports
- Check that sender and receiver use matching port numbers
- Verify network connectivity between machines

### Large Message Issues
- For messages > 32 MiB, increase MTU size in config (e.g., 512 or 1024 bytes)
- Or wait for extended packet offset support

## Directory Structure

```
SDR_RDMA_UDP/
├── sdr-udp/                    # UDP-based SDR prototype
│   ├── include/                # Header files
│   ├── src/                    # Implementation
│   ├── examples/               # Test programs (sender/receiver)
│   └── config/                 # Configuration files
├── external/rdmapp/            # RDMA++ library (C++20 coroutine-based RDMA)
│   ├── examples/              # RDMA examples
│   │   ├── rdma_reed_solomon.*  # RDMA FEC implementation
│   │   ├── send_bw.cc         # Bandwidth testing
│   │   └── write_bw.cc        # RDMA Write bandwidth test
│   ├── include/rdmapp/        # RDMA++ API headers
│   └── src/                    # RDMA++ implementation
├── docs/                       # Documentation and implementation guide
└── scripts/                    # Utility scripts
```

### RDMA Examples

The RDMA implementation includes several examples in `external/rdmapp/examples/`:

- **`rdma_reed_solomon.h/cpp`**: Complete RDMA FEC implementation with Reed-Solomon encoding
  - `RDMASender`: Sends data chunks with parity packets over RDMA
  - `RDMAReceiver`: Receives and decodes data using FEC
  - Chunked transmission aligned to MTU boundaries
  - Bitmap-based tracking of received packets

- **`send_bw.cc`**: Bandwidth testing using RDMA Send/Recv operations
- **`write_bw.cc`**: Bandwidth testing using RDMA Write operations
- **`acceptor.cc` / `connector.cc`**: Server/client connection establishment examples

## License

[Add your license information here]

## Contributing

This is a research prototype. For questions or contributions, please refer to the implementation guide in `docs/IMPLEMENTATION_GUIDE.md` or contact the research team.

## Acknowledgments

This project is part of ongoing research on Software-Defined Reliability for RDMA, enabling reliable cross-WAN communication for distributed AI training workloads.

---

**Status**: 🚧 Work in Progress - Research Prototype

