cmake_minimum_required(VERSION 3.10)
project(SDR_UDP)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Source files
set(SDR_SOURCES
    src/tcp_control.cpp
    src/sdr_api.cpp
    src/config_parser.cpp
)

# Create library
add_library(sdr_udp ${SDR_SOURCES})

# Original test examples
add_executable(sdr_test_receiver examples/sdr_test_receiver.cpp)
target_link_libraries(sdr_test_receiver sdr_udp pthread)

add_executable(sdr_test_sender examples/sdr_test_sender.cpp)
target_link_libraries(sdr_test_sender sdr_udp pthread)

# NEW: Selective Repeat test examples
add_executable(sdr_sr_receiver examples/sdr_sr_receiver.cpp)
target_link_libraries(sdr_sr_receiver sdr_udp pthread)

add_executable(sdr_sr_sender examples/sdr_sr_sender.cpp)
target_link_libraries(sdr_sr_sender sdr_udp pthread)

# Message ID test
add_executable(sdr_test_message_id examples/sdr_test_message_id.cpp)
target_link_libraries(sdr_test_message_id sdr_udp pthread)

# Installation
install(TARGETS sdr_udp sdr_test_receiver sdr_test_sender 
        sdr_sr_receiver sdr_sr_sender sdr_test_message_id
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)

install(DIRECTORY include/ DESTINATION include/sdr)