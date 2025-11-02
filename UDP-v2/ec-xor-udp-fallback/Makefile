# Compiler and flags
CXX = g++
# -Iinclude tells the compiler to look in the 'include' folder for headers
CXXFLAGS = -std=c++17 -O2 -pthread -Iinclude

# Source directories
SRC_DIR = src
BUILD_DIR = build

# --- Define EC Targets ---
EC_SENDER = $(BUILD_DIR)/ec_sender
EC_RECEIVER = $(BUILD_DIR)/ec_receiver

# --- Define Source Files ---
# Find all .cpp files in the src directory
SRCS = $(wildcard $(SRC_DIR)/*.cpp)
# Create a list of object files (e.g., build/ec_sender.o, build/udp_socket.o)
OBJS = $(SRCS:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

# --- Default rule: Build both programs ---
# THIS IS THE FIX: 'all' is now the first target and depends on both programs
all: $(EC_SENDER) $(EC_RECEIVER)

# --- Link all object files except the "other" main ---
$(EC_SENDER): $(filter-out $(BUILD_DIR)/ec_receiver.o, $(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^

$(EC_RECEIVER): $(filter-out $(BUILD_DIR)/ec_sender.o, $(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^

# --- Rule to compile .cpp files into .o (object) files ---
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)