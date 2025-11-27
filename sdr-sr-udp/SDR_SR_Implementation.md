Implementation Summary
Selective Repeat Protocol
The implementation follows the SDR-RDMA paper's approach:

Bitmap-Based Tracking

Backend bitmap: Tracks individual packet arrivals (atomic operations)
Frontend bitmap: Aggregates into chunks for efficiency
ACK bitmap: Sent from receiver to sender via TCP control channel


Window Management

Configurable window size (default: 16 packets)
Sliding window advances as ACKs arrive
Only unacked packets within window are retransmitted


Timeout & Retransmission

Per-packet RTO tracking (default: 100ms)
Packets retransmit when timeout expires
Retransmission counter prevents infinite loops


Control Flow

Initial burst: All packets sent as fast as possible
SR loop: Monitors ACKs, retransmits lost packets, slides window
Completion: COMPLETE_ACK signals successful transfer

SR Test File

Sends ALL 749 packets as fast as possible via UDP
No reliability - packets can be lost
Measures raw throughput (~700 Mbps on localhost)
Purpose: Simulates the initial "optimistic" transmission

-Parses ACK bitmap from receiver
-Updates per-packet ACK state
-Non-blocking TCP receive (50ms timeout)
-Window advances only when base packet is ACKed
-Cumulative ACK behavior (all packets before base must be ACKed)
-Allows out-of-order ACKs
-Bitmap accurately reflects received packets
-Handles partial reception (some packets missing)
-Sends COMPLETE_ACK when all packets received


SR Testing files commands

Terminal 1 - Receiver
./sdr_sr_receiver 8888 ../config.ini 1024

Terminal 2 - Sender:
bash./sdr_sr_sender 127.0.0.1 8888 1024

Run Large Transfer Test (10 MB)
Terminal 1
./sdr_sr_receiver 8888 ../config.ini 10240

Terminal 2
./sdr_sr_sender 127.0.0.1 8888 10240


Run with packet loss 
sudo tc qdisc add dev lo root netem loss 5%

# Run tests (will see retransmissions in action)
# Remove packet loss when done
sudo tc qdisc del dev lo root


NOTE: A lot of code was created with the help of Claude ai