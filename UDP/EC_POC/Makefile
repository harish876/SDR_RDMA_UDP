CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Iinclude
LDFLAGS = -pthread

# Directories
SRC_DIR = src
EXAMPLES_DIR = examples
BUILD_DIR = build

# Source files
SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

# Examples
EXAMPLES = simple_sender simple_receiver test_with_loss
EXAMPLE_OBJECTS = $(EXAMPLES:%=$(BUILD_DIR)/%.o)

# Default target
all: $(EXAMPLES)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Build library objects
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Build example objects
$(BUILD_DIR)/%.o: $(EXAMPLES_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Build examples
simple_sender: $(BUILD_DIR)/simple_sender.o $(OBJECTS)
	$(CXX) $^ -o $@ $(LDFLAGS)

simple_receiver: $(BUILD_DIR)/simple_receiver.o $(OBJECTS)
	$(CXX) $^ -o $@ $(LDFLAGS)

test_with_loss: $(BUILD_DIR)/test_with_loss.o $(OBJECTS)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(EXAMPLES)

# Install
install: $(EXAMPLES)
	mkdir -p /usr/local/bin
	cp $(EXAMPLES) /usr/local/bin/

# Test
test: test_with_loss
	@echo "Starting receiver in background..."
	./test_with_loss receiver &
	RECEIVER_PID=$$!; \
	sleep 2; \
	echo "Starting sender..."; \
	./test_with_loss sender 0.3; \
	kill $$RECEIVER_PID 2>/dev/null || true

# Help
help:
	@echo "Available targets:"
	@echo "  all          - Build all examples"
	@echo "  clean        - Remove build files"
	@echo "  test         - Run test with packet loss"
	@echo "  install      - Install to /usr/local/bin"
	@echo "  help         - Show this help"

.PHONY: all clean install test help
