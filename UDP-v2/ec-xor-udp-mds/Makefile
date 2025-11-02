# ============================================================
# Makefile for MDS Reed-Solomon Erasure Coding (ISA-L + UDP)
# ============================================================

CXX       := g++
CXXFLAGS  := -std=c++17 -O2 -pthread -Wall -Wextra -Iinclude -I/usr/include/isa-l
LDFLAGS   := -lisal
SRC_DIR   := src
BUILD_DIR := build

# Executables
EC_SENDER_EXE := $(BUILD_DIR)/ec_sender
EC_RECV_EXE   := $(BUILD_DIR)/ec_receiver

# Source files
EC_SENDER_SRC := $(SRC_DIR)/ec_sender.cpp $(SRC_DIR)/mds_ec.cpp $(SRC_DIR)/udp_socket.cpp
EC_RECV_SRC   := $(SRC_DIR)/ec_receiver.cpp $(SRC_DIR)/mds_ec.cpp $(SRC_DIR)/udp_socket.cpp

# ============================================================
# Rules
# ============================================================

.PHONY: all clean rebuild

all: $(EC_SENDER_EXE) $(EC_RECV_EXE)
	@echo "✅  Build complete."

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ---- Erasure Coding ----
$(EC_SENDER_EXE): $(EC_SENDER_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(EC_SENDER_SRC) $(LDFLAGS)

$(EC_RECV_EXE): $(EC_RECV_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(EC_RECV_SRC) $(LDFLAGS)

# ---- Cleaning ----
clean:
	@echo "🧹  Cleaning build files..."
	rm -rf $(BUILD_DIR)

rebuild: clean all
