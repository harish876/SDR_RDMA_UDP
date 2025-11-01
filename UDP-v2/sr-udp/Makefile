# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread

# Source directories
SRC_DIR = src
BUILD_DIR = build

# Targets
SENDER = $(BUILD_DIR)/sender
RECEIVER = $(BUILD_DIR)/receiver

# Source files
SENDER_SRC = $(SRC_DIR)/main.cpp $(SRC_DIR)/udp_transport.cpp
RECEIVER_SRC = $(SRC_DIR)/sr_protocol.cpp

# Default rule
all: $(SENDER) $(RECEIVER)

# Build sender executable
$(SENDER): $(SENDER_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Build receiver executable
$(RECEIVER): $(RECEIVER_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
