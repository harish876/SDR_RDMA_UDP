cmake_minimum_required(VERSION 3.10)
project(SDR_UDP)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Source files
set(SDR_SOURCES
    src/tcp_control.cpp
    src/sdr_api.cpp
    src/config_parser.cpp
    reliability/sr.cpp
    reliability/ec.cpp
)

# Create library
add_library(sdr_udp ${SDR_SOURCES})

# Examples
add_executable(sdr_test_receiver examples/sdr_test_receiver.cpp)
target_link_libraries(sdr_test_receiver sdr_udp pthread)
# Link filesystem library for std::filesystem (may be needed on some systems)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(sdr_test_receiver stdc++fs)
endif()

add_executable(sdr_test_sender examples/sdr_test_sender.cpp)
target_link_libraries(sdr_test_sender sdr_udp pthread)

# Installation
install(TARGETS sdr_udp sdr_test_receiver sdr_test_sender
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)

install(DIRECTORY include/ DESTINATION include/sdr)

