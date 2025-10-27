# Erasure Coding over UDP - Proof of Concept

This is a C++ implementation of erasure coding over UDP to handle packet loss and provide reliable data transmission.

## Features

- **Reed-Solomon Encoding**: Split data into k data packets + m parity packets
- **Packet Loss Recovery**: Can recover from up to m lost packets
- **UDP Communication**: Fast, connectionless transmission
- **Bitmap Tracking**: Efficient tracking of received packets
- **Configurable Parameters**: Adjustable k, m, packet size, timeouts
- **Statistics**: Detailed performance metrics

## Architecture

```
┌─────────────┬─────────────┬─────────────┬─────────────┐
│   Header    │  Sequence   │ Packet Type │    Data     │
│  (4 bytes)  │   (4 bytes) │  (1 byte)   │ (variable)  │
└─────────────┴─────────────┴─────────────┴─────────────┘
```

## Building

### Using CMake (Recommended)
```bash
cd UDP/EC_POC
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build .
```

### Using Makefile
```bash
cd UDP/EC_POC
make all
```

## Usage

### Basic Example

**Terminal 1 (Receiver):**
```bash
./simple_receiver 4950
```

**Terminal 2 (Sender):**
```bash
./simple_sender 127.0.0.1 4950
```

### Test with Packet Loss

**Terminal 1 (Receiver):**
```bash
./test_with_loss receiver
```

**Terminal 2 (Sender with 30% loss):**
```bash
./test_with_loss sender 0.3
```

## Configuration

The erasure coding parameters can be configured:

```cpp
Config config;
config.k = 8;           // Number of data packets
config.m = 2;           // Number of parity packets
config.packet_size = 1024;  // Size of each packet
config.timeout_ms = 1000;   // Retransmission timeout
config.max_retries = 3;     // Maximum retry attempts
config.enable_nack = true;  // Enable NACK-based retransmission
```

## Examples

### Simple Sender/Receiver
- `simple_sender.cpp`: Basic sender example
- `simple_receiver.cpp`: Basic receiver example

### Test with Loss
- `test_with_loss.cpp`: Comprehensive test with configurable packet loss

## Key Components

### UDPSender
- Encodes data using Reed-Solomon
- Sends packets over UDP
- Handles retransmissions
- Tracks statistics

### UDPReceiver
- Receives UDP packets
- Maintains bitmap of received packets
- Decodes data when enough packets arrive
- Sends ACK/NACK packets

### ReedSolomon
- Simple XOR-based parity implementation
- For production, use proper library like libfec

## Packet Types

- **DATA (0x01)**: Original data packets
- **PARITY (0x02)**: Parity packets for error correction
- **CONTROL (0x03)**: ACK/NACK packets

## Statistics

### Sender Stats
- Packets sent
- Bytes sent
- Retransmissions
- ACKs received
- NACKs received

### Receiver Stats
- Packets received
- Bytes received
- Packets decoded
- Packets lost
- ACKs sent
- NACKs sent

## Testing

Run the test with packet loss simulation:
```bash
make test
```

This will:
1. Start receiver in background
2. Send data with 30% packet loss
3. Verify successful recovery
4. Clean up processes

## Limitations

- Uses simple XOR-based parity (not true Reed-Solomon)
- No retransmission logic implemented
- Basic packet validation
- Single-threaded implementation

## Future Improvements

- Implement proper Reed-Solomon using libfec
- Add retransmission logic
- Multi-threaded receiver
- Better error handling
- Performance optimizations
- Integration with RDMA for high-performance scenarios
