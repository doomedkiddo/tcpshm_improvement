#!/bin/bash

# 设置颜色输出
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# 打印带颜色的消息
echo_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

echo_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

echo_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 显示帮助信息
show_help() {
    echo "加密货币市场数据传输示例构建脚本"
    echo
    echo "用法: $0 [选项]"
    echo
    echo "选项:"
    echo "  -h, --help             显示此帮助信息"
    echo "  -c, --clean            清理构建目录后重新构建"
    echo "  -d, --debug            以调试模式构建"
    echo "  -r, --release          以发布模式构建 (默认)"
    echo "  --run-client           构建成功后运行客户端示例程序"
    echo "  --run-server           构建成功后运行Echo服务器示例程序"
    echo "  --run-crypto-server    构建成功后运行加密货币市场数据服务器程序"
    echo "  --run                  构建成功后运行客户端和Echo服务器 (两个终端)"
    echo "  --run-crypto           构建成功后运行客户端和加密货币市场数据服务器 (两个终端)"
    echo "  --run-shm              构建成功后以共享内存模式运行客户端"
    echo
    echo "示例:"
    echo "  $0 --clean --debug     清理后以调试模式构建"
    echo "  $0 --run-client        构建并运行客户端示例程序"
    echo "  $0 --run-server        构建并运行Echo服务器示例程序"
    echo "  $0 --run-crypto-server 构建并运行加密货币市场数据服务器程序"
    echo "  $0 --run               构建并运行客户端和Echo服务器"
    echo "  $0 --run-crypto        构建并运行客户端和加密货币市场数据服务器"
    echo "  $0 --run-shm           构建并以共享内存模式运行客户端"
    echo
}

# 初始化变量
BUILD_TYPE="Release"
CLEAN_BUILD=0
RUN_CLIENT=0
RUN_SERVER=0
RUN_CRYPTO_SERVER=0
USE_SHM=0

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -c|--clean)
            CLEAN_BUILD=1
            shift
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -r|--release)
            BUILD_TYPE="Release"
            shift
            ;;
        --run-client)
            RUN_CLIENT=1
            shift
            ;;
        --run-server)
            RUN_SERVER=1
            shift
            ;;
        --run-crypto-server)
            RUN_CRYPTO_SERVER=1
            shift
            ;;
        --run)
            RUN_CLIENT=1
            RUN_SERVER=1
            shift
            ;;
        --run-crypto)
            RUN_CLIENT=1
            RUN_CRYPTO_SERVER=1
            shift
            ;;
        --run-shm)
            RUN_CLIENT=1
            USE_SHM=1
            shift
            ;;
        *)
            echo_error "未知选项: $1"
            show_help
            exit 1
            ;;
    esac
done

# 项目根目录
ROOT_DIR="$(pwd)"
BUILD_DIR="${ROOT_DIR}/build"

# 如果需要清理构建目录
if [ $CLEAN_BUILD -eq 1 ]; then
    echo_info "清理构建目录..."
    rm -rf "${BUILD_DIR}"
fi

# 创建构建目录（如果不存在）
if [ ! -d "${BUILD_DIR}" ]; then
    echo_info "创建构建目录..."
    mkdir -p "${BUILD_DIR}"
fi

# 进入构建目录
cd "${BUILD_DIR}" || { echo_error "无法进入构建目录!"; exit 1; }

# 运行CMake配置
echo_info "配置项目 (${BUILD_TYPE} 模式)..."
cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} .. || { echo_error "CMake配置失败!"; exit 1; }

# 编译项目
echo_info "编译项目..."
make -j$(nproc) || { echo_error "编译失败!"; exit 1; }

# 创建客户端和服务器目录（如果不存在）
if [ ! -d "${BUILD_DIR}/client" ]; then
    echo_info "创建客户端目录..."
    mkdir -p "${BUILD_DIR}/client"
fi

if [ ! -d "${BUILD_DIR}/server" ]; then
    echo_info "创建服务器目录..."
    mkdir -p "${BUILD_DIR}/server"
fi

sudo make install

# 如果需要运行Echo服务器
if [ $RUN_SERVER -eq 1 ]; then
    echo_info "启动Echo服务器..."
    # 在新的终端中运行服务器
    if [ "$(uname)" == "Darwin" ]; then
        # macOS
        open -a Terminal "${BUILD_DIR}/server_example"
    else
        # Linux
        if command -v gnome-terminal &> /dev/null; then
            gnome-terminal -- "${BUILD_DIR}/server_example"
        elif command -v xterm &> /dev/null; then
            xterm -e "${BUILD_DIR}/server_example" &
        elif command -v konsole &> /dev/null; then
            konsole -e "${BUILD_DIR}/server_example" &
        else
            echo_warn "找不到合适的终端模拟器，在后台运行服务器"
            "${BUILD_DIR}/server_example" &
        fi
    fi
    
    # 等待服务器启动
    echo_info "等待服务器启动 (2秒)..."
    sleep 2
fi

# 如果需要运行加密货币市场数据服务器
if [ $RUN_CRYPTO_SERVER -eq 1 ]; then
    echo_info "启动加密货币市场数据服务器..."
    # 在新的终端中运行服务器
    if [ "$(uname)" == "Darwin" ]; then
        # macOS
        open -a Terminal "${BUILD_DIR}/crypto_market_server"
    else
        # Linux
        if command -v gnome-terminal &> /dev/null; then
            gnome-terminal -- "${BUILD_DIR}/crypto_market_server"
        elif command -v xterm &> /dev/null; then
            xterm -e "${BUILD_DIR}/crypto_market_server" &
        elif command -v konsole &> /dev/null; then
            konsole -e "${BUILD_DIR}/crypto_market_server" &
        else
            echo_warn "找不到合适的终端模拟器，在后台运行服务器"
            "${BUILD_DIR}/crypto_market_server" &
        fi
    fi
    
    # 等待服务器启动
    echo_info "等待服务器启动 (2秒)..."
    sleep 2
fi

# 如果需要运行客户端
if [ $RUN_CLIENT -eq 1 ]; then
    echo_info "运行客户端..."
    # 添加运行参数
    RUN_ARGS=""
    if [ $USE_SHM -eq 1 ]; then
        echo_info "使用共享内存模式"
        RUN_ARGS="${RUN_ARGS} --shm"
    fi
    
    # 运行客户端
    ${BUILD_DIR}/client_example ${RUN_ARGS}
else
    if [ $RUN_SERVER -eq 0 ] && [ $RUN_CRYPTO_SERVER -eq 0 ]; then
        echo_info "构建成功!"
        echo_info "你可以运行:"
        echo "  Echo服务器: ${BUILD_DIR}/server_example"
        echo "  加密货币市场数据服务器: ${BUILD_DIR}/crypto_market_server"
        echo "  客户端: ${BUILD_DIR}/client_example"
        echo
        echo_info "或者通过以下命令使用共享内存模式:"
        echo "  ${BUILD_DIR}/client_example --shm"
    fi
fi

# 返回到原始目录
cd "${ROOT_DIR}" || { echo_error "无法返回到原始目录!"; exit 1; } 