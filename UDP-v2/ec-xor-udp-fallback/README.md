# EC-XOR-UDP with SR-Fallback

This project implements a hybrid reliability protocol based on the **SDR-RDMA** paper.  
It demonstrates a robust transfer over an unreliable network by combining two techniques:

1. **Erasure Coding (EC):** The sender first transmits all data chunks and XOR-based parity chunks in a "fire-and-forget" stream.
2. **Selective Repeat (SR) Fallback:** The receiver attempts to recover any lost data using the parity chunks. If decoding fails for any group of chunks, it sends a **Negative Acknowledgment (NACK)** to the sender, requesting a retransmission of only the specific missing data.

This hybrid approach provides the speed of EC for most packets and the reliability of SR for the few groups that fail.

---

## Prerequisites

* **Docker:** You must have Docker Desktop (for Mac/Windows) or Docker Engine (for Linux) installed and running.

---

## Build Instructions

The project is built inside a Docker container. Run this command from the project's root directory (the one containing your `Dockerfile`) to compile all the code.

```bash
docker build -t ec-xor-udp-fallback .
```

---

## How to Run the Simulation

To properly test the fallback mechanism, you must simulate a lossy network. This requires two separate terminals.

### Terminal 1: Start the Receiver & Network Simulation

**Start the Container:**
```bash
docker run -it --network host --cap-add=NET_ADMIN ec-xor-udp-fallback
```

**Simulate Packet Loss:**
```bash
tc qdisc replace dev lo root netem delay 25ms loss 5%
```

**Run the Receiver:**
```bash
./build/ec_receiver 9000
```
This terminal will now wait for the initial data, and will later send NACKs for any groups it can't recover.

---

### Terminal 2: Start the Sender

**Start a Second Container:**
```bash
docker run -it --network host --cap-add=NET_ADMIN ec-xor-udp-fallback
```

> Note: You do not need to run the tc command again. Both containers share the host's network, so the rule you set in Terminal 1 applies to all localhost traffic.

**Run the Sender:**
```bash
./build/ec_sender 127.0.0.1 9000
```

---

## Understanding the Output (Expected Log)

With 5% loss, you will see a "conversation" between the two terminals.

### 1. Sender (Terminal 2) - Initial Send
```
[EC Sender] Starting to send 128 groups to 127.0.0.1:9000
...
[EC Sender] Sent group 120/128
[EC Sender] ✅ Initial send complete (2546 ms).
[EC Sender] Now listening for ACKs/NACKs on port 8000...
```

### 2. Receiver (Terminal 1) - First Pass & Fallback
```
[EC Receiver] Listening on port 9000
...
[EC Receiver] ✅ Group 0 successfully recovered! (1/128)
...
[EC Receiver] ❌ Fallback Timer expired. Sending NACKs for failed groups.
[EC Receiver] Sending NACK for group 12
[EC Receiver] Sending NACK for group 33
```

### 3. Sender (Terminal 2) - Retransmission
```
[EC Sender] ❗️ Received NACK for group 12. Retransmitting...
[EC Sender]   -> Retransmitting data chunk 0 for group 12
[EC Sender] ❗️ Received NACK for group 33. Retransmitting...
[EC Sender]   -> Retransmitting data chunk 0 for group 33
```

### 4. Receiver (Terminal 1) - Final Recovery
```
[EC Receiver] ✅ Group 12 successfully recovered! (127/128)
[EC Receiver] ✅ Group 33 successfully recovered! (128/128)
[EC Receiver] ✅✅ All 128 groups recovered. Transfer complete.
```

### 5. Sender (Terminal 2) - Final ACK
```
[EC Sender] Received ACK for group 12 (127/128)
[EC Sender] Received ACK for group 33 (128/128)
[EC Sender] ✅✅ All 128 groups ACKed.
[EC Sender] Total retransmissions: 125
[EC Sender] Total time: 19831 ms.
```

> Note: Your exact number of retransmissions will vary based on the random 5% loss.

---

## How to Test on a Perfect Network

To prove your protocol works without loss, you must first remove the tc rules.

### In Terminal 1:
```bash
# Remove all network simulation rules
tc qdisc del dev lo root

# Run the receiver
./build/ec_receiver 9000
```

### In Terminal 2:
```bash
# Run the sender
./build/ec_sender 127.0.0.1 9000
```

This time, the receiver should recover all 128 groups on the first try.  
The sender will receive all 128 ACKs immediately and will report 0 retransmissions.

