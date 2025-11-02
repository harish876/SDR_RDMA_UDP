# EC-XOR-UDP Simulation (Without Fall Back) Guide

### Note
If you change your C++ code, you may need to run with `--no-cache` to force a complete rebuild:

```bash
docker build --no-cache -t ec-xor-udp .
```

---

## How to Run the Simulation

To simulate the network, you must run the sender and receiver in two separate terminals.

---

### Terminal 1: Start the Receiver & Network Simulation

**Start the Container:**  
Open your first terminal and run this command. This gives you a bash prompt inside the container with network admin privileges.

```bash
docker run -it --network host --cap-add=NET_ADMIN ec-xor-udp
```

**Simulate Packet Loss:**  
Inside the container, run the tc command. This is the most important step.  
We will add a 5% packet loss and a 25ms one-way delay (50ms RTT) to simulate a challenging network.

```bash
tc qdisc replace dev lo root netem delay 25ms loss 5%
```

**Run the Receiver:**  
Now, start the receiver. It will listen on port 9000 and wait for packets.

```bash
./build/ec_receiver 9000
```

This terminal will now wait, ready to receive and decode the transmission.

---

### Terminal 2: Start the Sender

**Start a Second Container:**  
Open a new, separate terminal window.

```bash
docker run -it --network host --cap-add=NET_ADMIN ec-xor-udp
```

> Note: You do **not** need to run the tc command again.  
> Both containers share the host's network, so the rule set in Terminal 1 applies to all localhost traffic.

**Run the Sender:**

```bash
./build/ec_sender 127.0.0.1 9000
```

The sender will send all 128 groups (1024 data chunks + 256 parity chunks) to the receiver and then exit.

---

## Understanding the Output

The logs you see will show whether your Erasure Coding was successful.

### Expected Output (with 5% Loss)

You will see that the Sender finishes quickly, but the Receiver may fail to recover all groups.

**Sender (Terminal 2) Output:**
```
[EC Sender] Starting to send 128 groups...
...
[EC Sender] Sent group 120/128
[EC Sender] ✅ All 128 groups sent (1282 ms).
```

**Receiver (Terminal 1) Output:**
```
[EC Receiver] Listening on port 9000
[EC Receiver] Expecting 128 groups (8 data, 2 parity per group).
[EC Receiver] ✅ Group 0 successfully recovered! (1/128)
[EC Receiver] ✅ Group 1 successfully recovered! (2/128)
[EC Receiver] ✅ Group 2 successfully recovered! (3/128)
[EC Receiver] ✅ Group 4 successfully recovered! (4/128)
...
[EC Receiver] ✅ Group 127 successfully recovered! (124/128)
[EC Receiver] ❌ Transfer finished, but 4 groups were unrecoverable...
```

---

## Why Does This Happen?

This is **not a bug**. This is the correct result.

Your (8, 2) XOR code is weak. It can only recover one lost packet from each "recovery group."  
With a 5% loss rate, it is statistically likely that two packets from the same group (e.g., chunk 1 and chunk 3) will be lost.  
When this happens, the decode function fails for that group, and the data is lost permanently.

---

## How to See It "Succeed"

To prove the code works, run the simulation again with a 1% loss rate.  
The (8, 2) code is strong enough to handle this.

**In Terminal 1:**
```bash
# Set a 1% loss rate instead
tc qdisc replace dev lo root netem delay 25ms loss 1%

# Run the receiver
./build/ec_receiver 9000
```

**In Terminal 2:**
```bash
# Run the sender
./build/ec_sender 127.0.0.1 9000
```

This time, your receiver should successfully recover all 128 groups, even with 1% of the packets being lost.

---

## How to Remove the Loss Condition

When you are finished testing, remove the network rules to return your computer to normal.

```bash
tc qdisc del dev lo root
```

If you re-run the simulation now (with 0% loss), the receiver will instantly recover all 128 groups, confirming your baseline works.

