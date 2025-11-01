#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <map>
#include <algorithm>
#include <fcntl.h>
#include <cerrno>

using namespace std;
using namespace chrono;

class UDPTransport {
public:
    int sockfd;
    int total_retransmissions = 0;
    int total_acks = 0;
    map<int, steady_clock::time_point> send_times;
    vector<double> rtt_samples;

    UDPTransport() {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = htons(8000);
        if (bind(sockfd, (const struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            perror("bind failed");
            exit(EXIT_FAILURE);
        }

        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags == -1) {
            perror("fcntl F_GETFL failed");
            exit(EXIT_FAILURE);
        }
        if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
            perror("fcntl F_SETFL O_NONBLOCK failed");
            exit(EXIT_FAILURE);
        }

        cout << "[info] UDP socket created, bound to port 8000, and set to non-blocking (fd=" << sockfd << ")\n";
    }

    ~UDPTransport() {
        close(sockfd);
        cout << "[info] UDP socket closed\n";
    }

    void run_sender(const string &receiver_ip, int receiver_port, int total_chunks) {
        struct sockaddr_in receiver_addr{};
        receiver_addr.sin_family = AF_INET;
        receiver_addr.sin_port = htons(receiver_port);

        inet_pton(AF_INET, receiver_ip.c_str(), &receiver_addr.sin_addr);

        vector<bool> ack_bitmap(total_chunks, false);
        int acked_count = 0;

        auto start_time = high_resolution_clock::now();
        cout << "[" << duration_cast<milliseconds>(start_time.time_since_epoch()).count()
             << "] [info] Bitmap tracking started for " << total_chunks << " chunks.\n";

        // <-- NEW LOGIC: We no longer "send all" first. -->
        // We track the next new chunk to send.
        int next_chunk_to_send = 0;
        
        // We track the last time we checked for retransmissions.
        auto last_rto_check = steady_clock::now();
        const auto RTO_DURATION = milliseconds(100); // 100ms RTO

        fd_set readfds;
        struct timeval tv{};

        while (acked_count < total_chunks) {
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);
            
            // <-- NEW LOGIC: Use a tiny 1ms timeout for the main loop -->
            // This lets us check for ACKs, send new packets, and check RTOs
            // in a balanced way.
            tv.tv_sec = 0;
            tv.tv_usec = 1000; // 1ms timeout

            int activity = select(sockfd + 1, &readfds, nullptr, nullptr, &tv);
            auto now = steady_clock::now();

            if (activity > 0 && FD_ISSET(sockfd, &readfds)) {
                // --- 1. DRAIN ACK BUFFER ---
                while (true) {
                    char buffer[256];
                    struct sockaddr_in ack_addr{};
                    socklen_t addr_len = sizeof(ack_addr);
                    ssize_t len = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                                           (struct sockaddr *)&ack_addr, &addr_len);
                    
                    if (len <= 0) {
                        if (errno == EWOULDBLOCK || errno == EAGAIN) break; 
                        break;
                    }

                    try {
                        int ack_id = stoi(string(buffer, len));
                        if (ack_id >= 0 && ack_id < total_chunks && !ack_bitmap[ack_id]) {
                            ack_bitmap[ack_id] = true;
                            acked_count++;
                            total_acks = acked_count;
                            
                            double rtt_ms = duration_cast<microseconds>(now - send_times[ack_id]).count() / 1000.0;
                            rtt_samples.push_back(rtt_ms);

                            if (acked_count % 100 == 0 || acked_count == total_chunks) {
                                cout << "[" << duration_cast<milliseconds>(now.time_since_epoch()).count()
                                     << "] [progress] ACK coverage: "
                                     << fixed << setprecision(1)
                                     << (100.0 * acked_count / total_chunks) << "% ("
                                     << acked_count << "/" << total_chunks << ")\n";
                            }
                        }
                    } catch (const std::exception& e) { /* ignore bad packets */ }
                } 
            } else if (activity < 0) {
                perror("select() error");
                break;
            }

            // --- 2. SEND NEW PACKETS (PACED) ---
            if (next_chunk_to_send < total_chunks) {
                string packet = "CHUNK_" + to_string(next_chunk_to_send);
                sendto(sockfd, packet.c_str(), packet.size(), 0,
                       (const struct sockaddr *)&receiver_addr, sizeof(receiver_addr));
                send_times[next_chunk_to_send] = now;
                
                if (next_chunk_to_send == 0 || (next_chunk_to_send+1) % 200 == 0 || next_chunk_to_send == total_chunks - 1) {
                    cout << "[data] Sent chunk " << next_chunk_to_send << " (original transmission)\n";
                }
                next_chunk_to_send++;

                // We add a tiny sleep to simulate a bottleneck,
                // but this is much smaller since we're also receiving.
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            
            // --- 3. CHECK FOR TIMEOUTS (RTO) ---
            if (duration_cast<milliseconds>(now - last_rto_check) > RTO_DURATION) {
                last_rto_check = now; // Reset RTO timer
                
                vector<int> missing;
                for (int i = 0; i < next_chunk_to_send; ++i) { // Only check chunks we've actually sent
                    if (!ack_bitmap[i]) {
                        // Check if the RTO has *actually* expired for this specific packet
                        if (duration_cast<milliseconds>(now - send_times[i]) > RTO_DURATION) {
                            missing.push_back(i);
                        }
                    }
                }
                
                if (!missing.empty()) {
                    total_retransmissions += missing.size();
                    auto now_ms = duration_cast<milliseconds>(
                        high_resolution_clock::now().time_since_epoch()).count();
                    
                    cout << "[" << now_ms << "] [retransmit] RTO expired. Retransmitting " << missing.size()
                         << " chunks, e.g. chunk " << missing[0] << "\n";

                    // Retransmit as a burst (correct for SR)
                    for (int m : missing) {
                        string packet = "CHUNK_" + to_string(m);
                        sendto(sockfd, packet.c_str(), packet.size(), 0,
                               (const struct sockaddr *)&receiver_addr, sizeof(receiver_addr));
                        send_times[m] = now; // Update the send time
                    }
                }
            }
        } // --- END of the while(acked_count < total_chunks) loop

        auto end_time = high_resolution_clock::now();
        double total_duration = duration_cast<milliseconds>(end_time - start_time).count();
        double avg_rtt = rtt_samples.empty() ? 0 : (accumulate(rtt_samples.begin(), rtt_samples.end(), 0.0) / rtt_samples.size());
        
        cout << "[" << duration_cast<milliseconds>(end_time.time_since_epoch()).count()
             << "] [info] Bitmap tracking complete, all chunks ACKed.\n";
        cout << fixed << setprecision(3);
        if (!rtt_samples.empty()) {
            auto [min_it, max_it] = minmax_element(rtt_samples.begin(), rtt_samples.end());
            cout << "[metrics] RTT (avg/min/max): " << avg_rtt << "/"
                 << *min_it << "/" << *max_it << " ms\n";
        }
        cout << "[metrics] Total retransmissions: " << total_retransmissions << "\n";
        cout << "[metrics] Total transfer duration: " << total_duration << " ms\n";
        cout << "[metrics] Effective throughput: "
             << (total_chunks * 1.0 / (total_duration / 1000.0)) << " Chunks/s ("
             << (total_chunks * 1.0 * 1024.0 / (total_duration / 1000.0)) / 1024.0 << " KB/s)\n";
    }
};