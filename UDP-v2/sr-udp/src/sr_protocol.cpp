#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <string>
#include <thread> // <-- FINAL FIX: Required for this_thread

using namespace std;
using namespace chrono;

class SRProtocolReceiver {
public:
    int sockfd;

    SRProtocolReceiver() {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in servaddr{};
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = INADDR_ANY;
        servaddr.sin_port = htons(9000); // Listen on port 9000

        if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
            perror("bind failed");
            exit(EXIT_FAILURE);
        }

        cout << "[receiver] Listening on port 9000...\n";
    }

    ~SRProtocolReceiver() {
        close(sockfd);
    }

    void run_receiver() {
        // <-- FINAL FIX: Use a bitmap to track *unique* chunks -->
        const int TOTAL_CHUNKS = 1024;
        vector<bool> received_bitmap(TOTAL_CHUNKS, false);
        int unique_chunks_received = 0;

        const int BUF_SIZE = 256;
        char buffer[BUF_SIZE];
        struct sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        auto start = high_resolution_clock::now();

        // <-- FINAL FIX: Loop until all *unique* chunks are in -->
        while (unique_chunks_received < TOTAL_CHUNKS) { 
            ssize_t n = recvfrom(sockfd, buffer, BUF_SIZE, 0,
                                 (struct sockaddr *)&client_addr, &len);
            socklen_t client_len = sizeof(client_addr);
            if (n <= 0) continue;
            
            string data(buffer, n);
            if (data.rfind("CHUNK_", 0) == 0) {
                try {
                    int chunk_id = stoi(data.substr(6));

                    if (chunk_id >= 0 && chunk_id < TOTAL_CHUNKS) {
                        // <-- FINAL FIX: Only count if it's a *new* chunk -->
                        if (!received_bitmap[chunk_id]) {
                            received_bitmap[chunk_id] = true;
                            unique_chunks_received++;

                            if (unique_chunks_received % 100 == 0) {
                                auto now = high_resolution_clock::now();
                                cout << "[" << duration_cast<milliseconds>(now.time_since_epoch()).count()
                                     << "] [receiver] Received " << unique_chunks_received << "/" << TOTAL_CHUNKS << " unique chunks.\n";
                            }
                        }

                        // Send ACK (we still do this for every packet)
                        string ack = to_string(chunk_id);
                        sendto(sockfd, ack.c_str(), ack.size(), 0, (const struct sockaddr *)&client_addr, client_len);

                        // <-- FINAL FIX: Add a small delay to prevent ACK storm -->
                        // This throttles the receiver so it doesn't flood the sender's socket buffer.
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                } catch (const std::exception& e) {
                    cout << "[receiver] Error parsing chunk ID from data: " << data << endl;
                }
            }
        }
        // <-- End of fixes -->

        // After the loop, we are done
        auto end = high_resolution_clock::now();
        double duration = duration_cast<milliseconds>(end - start).count();
        cout << "[receiver] ✅ All " << TOTAL_CHUNKS << " unique chunks received successfully (" << duration << " ms)\n";
        
        // Linger for 1 second to send final ACKs, ensuring the sender gets the message
        auto linger_start = high_resolution_clock::now();
        while(duration_cast<milliseconds>(high_resolution_clock::now() - linger_start).count() < 1000) {
             string ack = to_string(TOTAL_CHUNKS - 1); // Send ACK for the last chunk
             sendto(sockfd, ack.c_str(), ack.size(), 0, (const struct sockaddr *)&client_addr, sizeof(client_addr));
             std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        cout << "[receiver] Linger complete. Exiting.\n";
    }
};

int main() {
    SRProtocolReceiver receiver;
    receiver.run_receiver();
    return 0;
}