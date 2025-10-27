cmake_minimum_required(VERSION 3.10)
project(EC_POC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable compile_commands.json generation for LSP support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include directories
include_directories(include)

# Source files
set(SOURCES
    src/packet.cpp
    src/reed_solomon.cpp
    src/udp_sender.cpp
    src/udp_receiver.cpp
    src/utils.cpp
)

# Create library
add_library(ec_poc ${SOURCES})

# Examples
add_executable(simple_sender examples/simple_sender.cpp)
target_link_libraries(simple_sender ec_poc)

add_executable(simple_receiver examples/simple_receiver.cpp)
target_link_libraries(simple_receiver ec_poc)

add_executable(test_with_loss examples/test_with_loss.cpp)
target_link_libraries(test_with_loss ec_poc)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG")

# Install targets
install(TARGETS ec_poc simple_sender simple_receiver test_with_loss
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
