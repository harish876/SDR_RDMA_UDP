# SDR UDP - Reliable UDP Data Transfer Library

A high-performance UDP-based data transfer library with reliable delivery semantics, inspired by RDMA-style chunk-based transfer mechanisms. The library uses TCP for control/connection establishment and UDP for high-speed data transfer with chunk-level tracking.

## Features

- **High-Performance UDP Transfer**: Uses UDP for data transfer while maintaining reliability through chunk-level tracking
- **Lock-Free Bitmap Tracking**: Lock-free atomic operations for packet and chunk tracking
- **Chunk-Based Delivery**: Transfers data in chunks, allowing progress tracking at the chunk level
- **Thread-Safe Operations**: Safe for concurrent access with proper synchronization
- **TCP Control Channel**: Uses TCP for reliable connection establishment and control messages
- **Polling-Based Progress**: Frontend polling mechanism to track chunk completion status

## Building

### Prerequisites

- CMake 3.10 or higher
- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- pthread library (usually included on Unix systems)

### Build Steps

```bash
# Create build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build the project
make

# (Optional) Install the library
make install
```

This will create:
- `libsdr_udp.a` - The static library
- `sdr_test_receiver` - Receiver test executable
- `sdr_test_sender` - Sender test executable

## Testing

The project includes two test programs that demonstrate basic send/receive functionality.

### Quick Start Test

**Terminal 1 - Start Receiver:**
```bash
cd build
./sdr_test_receiver <tcp_port> <udp_port> [message_size] [config_file]
```

Example:
```bash
./sdr_test_receiver 8888 9999 1048576
# Or with explicit config file:
./sdr_test_receiver 8888 9999 1048576 ../config/receiver.config
```
This starts a receiver listening on:
- TCP port 8888 for control/connection
- UDP port 9999 for data
- Expects a 1 MiB message (1048576 bytes)
- Config file: optional path to `.config` file (defaults to `../config/receiver.config` if not specified)

**Terminal 2 - Start Sender:**
```bash
cd build
./sdr_test_sender <server_ip> <tcp_port> <udp_port> [message_size]
```

Example:
```bash
./sdr_test_sender 127.0.0.1 8888 9999 1048576
```
This connects to the receiver and sends:
- 1 MiB of data
- Server IP: 127.0.0.1 (localhost)
- TCP port: 8888
- UDP port: 9999

### Expected Output

**Receiver Output:**
```
[Receiver] Starting SDR receiver...
[Receiver] TCP port: 8888
[Receiver] UDP port: 9999
[Receiver] Expected message size: 1048576 bytes
[Receiver] Waiting for sender connection...
[Receiver] Connection accepted!
[Receiver] Receive posted, waiting for data...
[Receiver] Chunks received: 0/16384
[Receiver] Chunks received: 10/16384
...
[Receiver] All chunks received!
[Receiver] Transfer completed in XXX ms
[Receiver] Data verification: PASSED
[Receiver] Done!
```

**Sender Output:**
```
[Sender] Starting SDR sender...
[Sender] Server: 127.0.0.1:8888
[Sender] UDP port: 9999
[Sender] Message size: 1048576 bytes
[Sender] Connected!
[Sender] Sending message...
[Sender] Sent XXXX packets in XXX ms
[Sender] Done!
```

### Testing Scenarios

#### 1. Small Message Test
```bash
# Receiver
./sdr_test_receiver 8888 9999 1024
# Or with config file:
./sdr_test_receiver 8888 9999 1024 ../config/receiver.config

# Sender (in another terminal)
./sdr_test_sender 127.0.0.1 8888 9999 1024
```

#### 2. Large Message Test
```bash
# Receiver
./sdr_test_receiver 8888 9999 10485760  # 10 MiB
# Or with config file:
./sdr_test_receiver 8888 9999 10485760 ../config/receiver.config

# Sender
./sdr_test_sender 127.0.0.1 8888 9999 10485760
```

#### 3. Network Testing (Different Machines)

On Machine A (Receiver):
```bash
./sdr_test_receiver 8888 9999 1048576
# Or with config file:
./sdr_test_receiver 8888 9999 1048576 ../config/receiver.config
# Note the machine's IP address (use ifconfig or ip addr)
```

On Machine B (Sender):
```bash
./sdr_test_sender <Machine_A_IP> 8888 9999 1048576
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

// Accept connection (after listen)
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

// Complete receive
sdr_recv_complete(recv_handle);
```

### Send Operations (One-Shot)

```cpp
// Send data
const void* buffer = data;
size_t length = data_size;
SDRSendHandle* send_handle = nullptr;
sdr_send_post(conn, buffer, length, &send_handle);

// Poll for completion
sdr_send_poll(send_handle);
```

### Send Operations (Streaming)

```cpp
// Start streaming send
SDRStreamHandle* stream_handle = nullptr;
sdr_send_stream_start(conn, buffer, total_length, initial_offset, &stream_handle);

// Continue sending chunks
sdr_send_stream_continue(stream_handle, offset, length);

// End streaming
sdr_send_stream_end(stream_handle);
```

### Cleanup

```cpp
// Disconnect
sdr_disconnect(conn);

// Free handles
delete recv_handle;
delete send_handle;
delete stream_handle;
```

## Architecture

### Components

1. **BackendBitmap**: Tracks individual packet reception using lock-free atomic operations
2. **FrontendBitmap**: Polls backend bitmap and maintains chunk-level completion status
3. **UDPReceiver**: Handles incoming UDP packets and updates backend bitmap
4. **TCPControl**: Manages TCP connection for control messages (CTS/connection setup)
5. **ConnectionContext**: Manages per-connection state and message tracking

### Data Flow

1. **Connection Setup**: 
   - Receiver calls `sdr_listen()` and accepts connection via TCP
   - Sender calls `sdr_connect()` to establish TCP connection
   
2. **Transfer Initiation**:
   - Receiver posts receive buffer with `sdr_recv_post()`
   - Sender posts send with `sdr_send_post()`
   - Receiver sends CTS (Clear To Send) via TCP

3. **Data Transfer**:
   - Sender sends UDP packets in chunks
   - Receiver processes UDP packets and updates backend bitmap
   - Frontend bitmap polls backend and tracks chunk completion

4. **Completion**:
   - Receiver polls chunk bitmap for completion
   - Sender polls send handle for completion
   - Both sides clean up resources

## Configuration

### Configuration File

The receiver accepts an optional configuration file path as the 4th command-line argument. If not provided, it defaults to `../config/receiver.config` (relative to the executable directory).

The configuration file uses a simple key-value format:
```
# Comments start with #
mtu_bytes=128
packets_per_chunk=32
transfer_id=1
```

### Connection Parameters

The library uses connection parameters that can be configured via the config file (or programmatically during connection establishment):

- `mtu_bytes`: Maximum transmission unit size (bytes per packet)
- `packets_per_chunk`: Number of packets per chunk
- `udp_server_port`: UDP port for data transfer (set via command line)
- `udp_server_ip`: IP address for UDP server
- `transfer_id`: Unique transfer identifier

Example config file (`config/receiver.config`):
```
# Maximum Transmission Unit (MTU) - packet payload size in bytes
mtu_bytes=128

# Number of packets per chunk
packets_per_chunk=32

# Transfer ID (optional, defaults to 1)
transfer_id=1
```

## Troubleshooting

### Port Already in Use

If you see errors about ports being in use:
```bash
# Find and kill processes using the port
lsof -ti:8888 | xargs kill -9  # For TCP port
lsof -ti:9999 | xargs kill -9  # For UDP port
```

### Connection Timeout

- Ensure firewall allows TCP and UDP traffic on the specified ports
- Check that both sender and receiver use matching port numbers
- Verify network connectivity between machines

### Build Errors

- Ensure C++17 support is enabled
- Check that pthread library is available: `ldconfig -p | grep pthread`
- On macOS, ensure Xcode command-line tools are installed

## License

[Add your license information here]

## Contributing

[Add contribution guidelines here]

