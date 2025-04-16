#include "market_data_server.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

using namespace crypto;

// 全局运行标志
std::atomic<bool> g_running{true};

// 信号处理
void signal_handler(int sig) {
    std::cout << "接收到信号 " << sig << "，准备退出..." << std::endl;
    g_running = false;
}

int main(int argc, char** argv) {
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 默认参数
    std::string server_name = "CryptoServer";
    uint16_t server_port = 8888;
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--name" && i + 1 < argc) {
            server_name = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--help") {
            std::cout << "使用方法: " << argv[0] << " [选项]" << std::endl;
            std::cout << "选项:" << std::endl;
            std::cout << "  --name NAME    服务器名称 (默认: CryptoServer)" << std::endl;
            std::cout << "  --port PORT    服务器监听端口 (默认: 8888)" << std::endl;
            std::cout << "  --help         显示此帮助信息" << std::endl;
            return 0;
        }
    }
    
    std::cout << "加密货币市场数据服务器" << std::endl;
    std::cout << "服务器名称: " << server_name << std::endl;
    std::cout << "监听端口: " << server_port << std::endl;
    
    // 使用通用数据目录
    std::string data_dir = "/tmp/crypto_data";
    
    // 创建目录并设置权限
    system(("mkdir -p " + data_dir).c_str());
    system(("chmod 777 " + data_dir).c_str());
    
    std::cout << "使用数据目录: " << data_dir << std::endl;
    
    // 创建服务器实例
    MarketDataServer server(server_name, data_dir);
    server.SetPort(server_port);
    
    // 启动服务器
    if (!server.Start()) {
        std::cerr << "服务器启动失败" << std::endl;
        return 1;
    }
    
    std::cout << "服务器已启动，按Ctrl+C退出" << std::endl;
    
    // 短暂延迟以确保文件系统已就绪
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // 主循环 - 定期轮询服务器
    while (g_running) {
        // 轮询服务器
        server.Poll();
        
        // 睡眠一小段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "正在停止服务器..." << std::endl;
    
    // 停止服务器
    server.Stop();
    
    std::cout << "服务器已停止" << std::endl;
    return 0;
} 