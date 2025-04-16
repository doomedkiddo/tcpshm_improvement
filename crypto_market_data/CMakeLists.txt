cmake_minimum_required(VERSION 3.10)
project(crypto_market_data)

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -fsanitize=address")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-std=c++20)
endif()

# 寻找pthread库
find_package(Threads REQUIRED)

# 包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

# 创建客户端示例可执行文件
add_executable(client_example
    client_example.cpp
)

# 创建服务器示例可执行文件
add_executable(server_example
    server_example.cpp
)

# 链接pthread库
target_link_libraries(client_example 
    pthread
)

# 链接pthread库到服务器
target_link_libraries(server_example 
    pthread
)

# 添加自定义目标，用于创建客户端和服务器目录
add_custom_target(create_dirs ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/client
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/server
    COMMENT "Creating client and server directories"
)

# 添加依赖关系，确保目录创建在可执行文件生成之前
add_dependencies(client_example create_dirs)
add_dependencies(server_example create_dirs)

# 安装目标
install(TARGETS client_example server_example
    RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}
)

# 添加一个运行客户端的目标
add_custom_target(run_client
    COMMAND ./client_example
    DEPENDS client_example
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running client example..."
)

# 添加一个运行服务器的目标
add_custom_target(run_server
    COMMAND ./server_example
    DEPENDS server_example
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running server example..."
)

# 添加一个运行客户端的目标 (向后兼容)
add_custom_target(run
    COMMAND ./client_example
    DEPENDS client_example
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running client example..."
)

# 打印配置信息
message(STATUS "CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}")
message(STATUS "CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "CMAKE_CXX_FLAGS_DEBUG: ${CMAKE_CXX_FLAGS_DEBUG}")
message(STATUS "CMAKE_CXX_FLAGS_RELEASE: ${CMAKE_CXX_FLAGS_RELEASE}") 