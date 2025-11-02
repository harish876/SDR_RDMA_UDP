# EC-MDS-UDP: Reed-Solomon (MDS) Protocol Simulation

This project implements a high-performance, hybrid reliability protocol based on the **SDR-RDMA** paper.  
It uses the Intel **ISA-L** library to implement a powerful **MDS (Maximum Distance Separable)** Reed-Solomon Erasure Coding (EC) scheme, combined with a **Selective Repeat (SR)** NACK-based fallback.

This protocol is far more resilient to packet loss than a simple XOR code.  
This implementation uses a **(k=32, m=8)** configuration, meaning for every 32 data packets, it generates 8 parity packets.  
It can recover from any 8 packet losses within that 40-packet group, making it robust against high-loss networks.

---

## Simulation Components

* **Docker** – builds the C++ code and links the `libisal-dev` library.  
* **`tc` (traffic control)** – simulates a high-loss, high-latency WAN.

---

## Prerequisites

* **Docker:** You must have Docker Desktop (for Mac/Windows) or Docker Engine (for Linux) installed and running.

---

## Build Instructions

The project is built inside a Docker container.  
This command will also install the required `libisal-dev` dependency.

Run this command from the project's root directory:

```bash
docker build -t ec-xor-udp-mds .
```

**Note:** If you change your C++ code, you may need to run with `--no-cache` to force a complete rebuild:

```bash
docker build --no-cache -t ec-xor-udp-mds .
```

---

## How to Run the Simulation

To properly test the protocol, you must simulate a lossy network.  
This requires **two separate terminals.**

### Terminal 1: Start the Receiver & Network Simulation

**Start the Container:**
```bash
docker run -it --network host --cap-add=NET_ADMIN ec-xor-udp-mds
```

**Simulate Packet Loss:**
```bash
tc qdisc replace dev lo root netem delay 25ms loss 20%
```

> This high loss rate would have easily broken the old XOR project, but the (32, 8) MDS code can handle it.

**Run the Receiver:**
```bash
./build/ec_receiver 9000
```

This terminal will now wait, ready to receive and perform Reed-Solomon decoding.

---

### Terminal 2: Start the Sender

**Start a Second Container:**
```bash
docker run -it --network host --cap-add=NET_ADMIN ec-xor-udp-mds
```

> Note: You do not need to run the `tc` command again.

**Run the Sender:**
```bash
./build/ec_sender 127.0.0.1 9000
```

---

## Understanding the Output (Expected Log)

With a 20% loss rate, you should see the following:

### 1. Sender (Terminal 2) – Initial Send
```
[EC Sender] Starting to send 32 groups to 127.0.0.1:9000
...
[EC Sender] Sent group 32/32
[EC Sender] ✅ Initial send complete (1301 ms).
[EC Sender] Now listening for ACKs/NACKs on port 8000...
```

### 2. Receiver (Terminal 1) – First Pass Recovery
```
[EC Receiver] Listening on port 9000
...
[EC Receiver] ✅ Group 0 successfully recovered! (1/32)
[EC Receiver] ✅ Group 1 successfully recovered! (2/32)
...
```

### 3. Fallback (Possible)
It is statistically possible that more than 8 packets are lost in a single group.  
In this case, you will see the Fallback mechanism activate:

```
[EC Receiver] ❌ Fallback Timer expired. Sending NACKs for failed groups.
[EC Sender] ❗️ Received NACK for group X. Retransmitting...
[EC Receiver] ✅ Group X successfully recovered!
```

### 4. Final Output
Both terminals should report a 100% successful transfer:

```
[EC Receiver] ✅✅ All 32 groups recovered. Transfer complete.
[EC Sender] ✅✅ All 32 groups ACKed.
```

This proves the MDS code is far superior to the simple XOR code, as it can handle a 20% loss rate, relying on the SR-fallback only for extreme cases.

---

## How to Remove the Loss Condition

When you are finished testing, remove the network rules:

```bash
tc qdisc del dev lo root
```

