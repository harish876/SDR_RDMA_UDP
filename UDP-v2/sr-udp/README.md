# SR-UDP: A Selective Repeat (SR) Protocol Simulation

This project is a C++ implementation of a simple Selective Repeat (SR) protocol over UDP. It is designed to simulate the transfer of a large file (split into chunks) over an unreliable, high-latency Wide Area Network (WAN).

The simulation uses Docker to create a consistent environment and the Linux `tc` (traffic control) utility to artificially introduce packet loss and latency, allowing you to test the protocol's retransmission logic as described in networking papers.

---

## Prerequisites

* **Docker:** You must have Docker Desktop (for Mac/Windows) or Docker Engine (for Linux) installed and running.

---

## Setup: Building the Docker Image

1.  Open a terminal in the project's root directory (the one containing the `Dockerfile`).
2.  Build the Docker image. This will compile your C++ code inside the container.

    ```bash
    docker build -t sr-udp .
    ```

    * (Optional) If you make C++ code changes, you may want to use `--no-cache` to force a rebuild:
        `docker build --no-cache -t sr-udp .`

---

## How to Run the Simulation

To simulate a network, you must run the sender and receiver in **two separate terminals**.

### Step 1: Terminal 1 (Start the Receiver)

1.  Open your first terminal.
2.  Run the Docker container. This gives you a `bash` prompt inside the container.

    ```bash
    docker run -it --network host --cap-add=NET_ADMIN sr-udp
    ```
    * `--network host`: Allows the container to share your host's network (so `127.0.0.1` works).
    * `--cap-add=NET_ADMIN`: Gives the container permission to modify the network (which is required for the `tc` command).

3.  Inside the container, set up the "fake" WAN. This command adds a **25ms one-way delay** (50ms RTT) and a **1% packet loss** to all traffic on your `localhost` interface.

    ```bash
    tc qdisc replace dev lo root netem delay 25ms loss 1%
    ```

4.  Now, run the receiver program. It will wait for incoming packets.

    ```bash
    ./build/receiver
    ```
    *You can now leave this terminal running.*

### Step 2: Terminal 2 (Start the Sender)

1.  Open a **new, separate** terminal.
2.  Run the Docker container to get a second `bash` prompt.

    ```bash
    docker run -it --network host --cap-add=NET_ADMIN sr-udp
    ```
    *(Note: You do not need to run the `tc` command again. Since both containers use `--network host`, the rule you set in Terminal 1 already applies to all `localhost` traffic.)*

3.  Run the sender program to start the transfer.

    ```bash
    ./build/sender 127.0.0.1 9000 1024
    ```

    * **Arguments:** `build/sender <ip> <port> <chunks>`
        * `127.0.0.1`: The IP of the receiver.
        * `9000`: The port the receiver is listening on.
        * `1024`: The total number of chunks to send.

### Step 3: Observe the Results

* **In Terminal 2 (Sender):** You will see the sender pace its initial transmission. Then, you will see `[retransmit]` logs appear as its RTO (100ms) expires for packets that were lost (due to the 1% loss rule).
* **In Terminal 1 (Receiver):** You will see the receiver's progress as it successfully receives unique chunks.

The simulation will end when the sender has confirmed that all 1024 chunks have been received.

---

## Customizing the Simulation

You can test other network conditions by changing the `tc` command (in Terminal 1).

* **To simulate a 5% packet loss:**
    ```bash
    tc qdisc replace dev lo root netem delay 25ms loss 5%
    ```

* **To simulate a 100ms one-way delay (200ms RTT) with 2% loss:**
    ```bash
    tc qdisc replace dev lo root netem delay 100ms loss 2%
    ```

---

## Cleanup

1.  **Stop the Programs:** Press `Ctrl+C` in both terminals to stop the sender and receiver.
2.  **Reset Your Network:** Run this command in **one** of your container terminals to remove the network simulation rules. This is important, or your `localhost` will remain slow and lossy!

    ```bash
    tc qdisc del dev lo root
    ```

3.  **Exit the Containers:** Type `exit` in both terminals to close them.

---

## Project File Structure

Based on your `Makefile`, only a few of the files in the `src` directory are actively used for this simulation.

* `Makefile`: Defines how to build the `sender` and `receiver`.
* `Dockerfile`: Defines the container environment.
* `src/main.cpp`: The main entry point for the `sender`.
* `src/udp_transport.cpp`: The complete SR sender logic.
* `src/sr_protocol.cpp`: The complete SR receiver logic.

The other files (e.g., `sr_sender.cpp`, `sr_receiver.cpp`, `utils.h`) are not currently compiled or used by the `Makefile`.