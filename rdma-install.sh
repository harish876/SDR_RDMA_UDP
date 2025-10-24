#!/usr/bin/env bash
set -euo pipefail


# install RDMA userspace & NIC utilities
sudo apt update
sudo apt install -y rdma-core ibverbs-providers perftest mstflint ethtool iperf3 ibverbs-utils rdmacm-utils

