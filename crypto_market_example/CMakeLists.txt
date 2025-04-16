cmake_minimum_required(VERSION 3.10)
project(tcpshm_test CXX)

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce C++20 with compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-std=c++20)
endif()

# Add optimization flags for Release build
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# 定义宏以禁用系统头文件中的extern "C"
add_compile_definitions(__BEGIN_DECLS="#define DO_NOTHING")
add_compile_definitions(__END_DECLS="#define DO_NOTHING")

# Add executable targets
add_executable(echo_server echo_server.cpp)
add_executable(echo_client echo_client.cpp)

# Link libraries
target_link_libraries(echo_server PRIVATE pthread rt)
target_link_libraries(echo_client PRIVATE pthread rt)

# Include directories
include_directories(..)

# Install binaries to bin directory
install(TARGETS echo_server echo_client
        RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}) 