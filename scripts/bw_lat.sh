#!/usr/bin/env bash
set -euo pipefail

# Bandwidth and Latency Testing Script
# Supports TCP, UDP, and RDMA (ib_send_bw, ib_write_bw) tests
#
# Usage:
#   Server mode: ./bw_lat.sh server [tcp|udp|rdma_send|rdma_write]
#   Client mode: ./bw_lat.sh client <server_ip> [tcp|udp|rdma_send|rdma_write|rdma|all]

MODE="${1:-}"
SERVER_IP="${2:-}"
TEST_TYPE="${3:-all}"
NUM_QPS="${NUM_QPS:-1}"  # Number of QPs for RDMA tests (default 1, can be overridden with NUM_QPS env var)

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}\n"
}

print_error() {
    echo -e "${RED}ERROR: $1${NC}" >&2
}

print_info() {
    echo -e "${YELLOW}INFO: $1${NC}"
}

check_tool() {
    if ! command -v "$1" &> /dev/null; then
        print_error "$1 not found. Please install it."
        return 1
    fi
}

# Server functions
run_server_tcp() {
    print_header "Starting TCP Server (qperf)"
    print_info "Waiting for client connections..."
    qperf
}

run_server_udp() {
    print_header "Starting UDP Server (qperf)"
    print_info "Waiting for client connections..."
    qperf
}

run_server_rdma_send() {
    print_header "Starting RDMA Send Server (ib_send_bw)"
    print_info "Waiting for client connections with $NUM_QPS QP(s)..."
    ib_send_bw -q "$NUM_QPS"
}

run_server_rdma_write() {
    print_header "Starting RDMA Write Server (ib_write_bw)"
    print_info "Waiting for client connections with $NUM_QPS QP(s)..."
    ib_write_bw -q "$NUM_QPS"
}

run_server_all() {
    print_error "Server mode with 'all' not supported. Please run servers separately."
    echo "Run:"
    echo "  ./bw_lat.sh server tcp        (in one terminal)"
    echo "  ./bw_lat.sh server udp        (in another terminal)"
    echo "  ./bw_lat.sh server rdma_send  (for RDMA send tests)"
    echo "  ./bw_lat.sh server rdma_write (for RDMA write tests)"
    exit 1
}

# Client functions
run_client_tcp() {
    print_header "Running TCP Tests (qperf)"
    check_tool qperf || return 1
    
    print_info "TCP Bandwidth Test..."
    qperf -t 10 "$SERVER_IP" tcp_bw || print_error "TCP bandwidth test failed"
    
    print_info "TCP Latency Test..."
    qperf -t 10 "$SERVER_IP" tcp_lat || print_error "TCP latency test failed"
    
    print_info "TCP Bidirectional Bandwidth Test..."
    qperf -t 10 "$SERVER_IP" tcp_bi_bw || print_error "TCP bidirectional bandwidth test failed"
}

run_client_udp() {
    print_header "Running UDP Tests (qperf)"
    check_tool qperf || return 1
    
    print_info "UDP Bandwidth Test..."
    qperf -t 10 "$SERVER_IP" udp_bw || print_error "UDP bandwidth test failed"
    
    print_info "UDP Latency Test..."
    qperf -t 10 "$SERVER_IP" udp_lat || print_error "UDP latency test failed"
    
    print_info "UDP Bidirectional Bandwidth Test..."
    qperf -t 10 "$SERVER_IP" udp_bi_bw || print_error "UDP bidirectional bandwidth test failed"
}

run_client_rdma_send() {
    print_header "Running RDMA Send Bandwidth Test (ib_send_bw)"
    check_tool ib_send_bw || return 1
    
    if [[ "$NUM_QPS" -gt 1 ]]; then
        print_info "Note: Server must also be started with $NUM_QPS QP(s) (use NUM_QPS=$NUM_QPS on server)"
    fi
    print_info "Running ib_send_bw to $SERVER_IP with $NUM_QPS QP(s)..."
    ib_send_bw -q "$NUM_QPS" "$SERVER_IP" || print_error "RDMA send bandwidth test failed"
}

run_client_rdma_write() {
    print_header "Running RDMA Write Bandwidth Test (ib_write_bw)"
    check_tool ib_write_bw || return 1
    
    if [[ "$NUM_QPS" -gt 1 ]]; then
        print_info "Note: Server must also be started with $NUM_QPS QP(s) (use NUM_QPS=$NUM_QPS on server)"
    fi
    print_info "Running ib_write_bw to $SERVER_IP with $NUM_QPS QP(s)..."
    ib_write_bw -q "$NUM_QPS" "$SERVER_IP" || print_error "RDMA write bandwidth test failed"
}

run_client_rdma_latency() {
    print_header "Running RDMA Latency Tests"
    
    if check_tool ib_send_lat; then
        print_info "RDMA Send Latency Test..."
        ib_send_lat "$SERVER_IP" || print_error "RDMA send latency test failed"
    fi
    
    if check_tool ib_write_lat; then
        print_info "RDMA Write Latency Test..."
        ib_write_lat "$SERVER_IP" || print_error "RDMA write latency test failed"
    fi
}

run_client_rdma() {
    print_info "Running all RDMA tests (send, write, and latency)"
    run_client_rdma_send
    run_client_rdma_write
    run_client_rdma_latency
}

run_client_all() {
    print_header "Running All Tests to $SERVER_IP"
    run_client_tcp
    run_client_udp
    run_client_rdma
}

# Main logic
if [[ "$MODE" == "server" ]]; then
    case "$TEST_TYPE" in
        tcp)
            run_server_tcp
            ;;
        udp)
            run_server_udp
            ;;
        rdma_send)
            run_server_rdma_send
            ;;
        rdma_write)
            run_server_rdma_write
            ;;
        all)
            run_server_all
            ;;
        *)
            print_error "Unknown test type: $TEST_TYPE"
            echo "Usage: $0 server [tcp|udp|rdma_send|rdma_write]"
            exit 1
            ;;
    esac
elif [[ "$MODE" == "client" ]]; then
    if [[ -z "$SERVER_IP" ]]; then
        print_error "Server IP required in client mode"
        echo "Usage: $0 client <server_ip> [tcp|udp|rdma|all]"
        exit 1
    fi
    
    print_info "Connecting to server: $SERVER_IP"
    
    case "$TEST_TYPE" in
        tcp)
            run_client_tcp
            ;;
        udp)
            run_client_udp
            ;;
        rdma_send)
            run_client_rdma_send
            ;;
        rdma_write)
            run_client_rdma_write
            ;;
        rdma)
            run_client_rdma
            ;;
        all)
            run_client_all
            ;;
        *)
            print_error "Unknown test type: $TEST_TYPE"
            echo "Usage: $0 client <server_ip> [tcp|udp|rdma_send|rdma_write|rdma|all]"
            exit 1
            ;;
    esac
else
    print_error "Invalid mode: $MODE"
    echo ""
    echo "Usage:"
    echo "  Server mode: $0 server [tcp|udp|rdma_send|rdma_write]"
    echo "  Client mode: $0 client <server_ip> [tcp|udp|rdma_send|rdma_write|rdma|all]"
    echo ""
    echo "Examples:"
    echo "  # Start TCP server"
    echo "  $0 server tcp"
    echo ""
    echo "  # Start UDP server"
    echo "  $0 server udp"
    echo ""
    echo "  # Start RDMA Send server (default 1 QP)"
    echo "  $0 server rdma_send"
    echo ""
    echo "  # Start RDMA Send server with 4 QPs"
    echo "  NUM_QPS=4 $0 server rdma_send"
    echo ""
    echo "  # Start RDMA Write server (default 1 QP)"
    echo "  $0 server rdma_write"
    echo ""
    echo "  # Start RDMA Write server with 8 QPs"
    echo "  NUM_QPS=8 $0 server rdma_write"
    echo ""
    echo "  # Run TCP tests from client"
    echo "  $0 client 128.110.219.167 tcp"
    echo ""
    echo "  # Run UDP tests from client"
    echo "  $0 client 128.110.219.167 udp"
    echo ""
    echo "  # Run RDMA Send test from client (requires ib_send_bw server running)"
    echo "  $0 client 128.110.219.167 rdma_send"
    echo ""
    echo "  # Run RDMA Send test with 4 QPs for higher bandwidth"
    echo "  NUM_QPS=4 $0 client 128.110.219.167 rdma_send"
    echo ""
    echo "  # Run RDMA Write test from client (requires ib_write_bw server running)"
    echo "  $0 client 128.110.219.167 rdma_write"
    echo ""
    echo "  # Run RDMA Write test with 8 QPs for higher bandwidth"
    echo "  NUM_QPS=8 $0 client 128.110.219.167 rdma_write"
    echo ""
    echo "  # Run all RDMA tests from client (requires both servers running)"
    echo "  $0 client 128.110.219.167 rdma"
    echo ""
    echo "  # Run all tests from client"
    echo "  $0 client 128.110.219.167 all"
    exit 1
fi

