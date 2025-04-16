#include "market_data_client.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

using namespace crypto;

// 全局运行标志
std::atomic<bool> g_running{true};

// 信号处理
void signal_handler(int sig) {
    std::cout << "接收到信号 " << sig << "，准备退出..." << std::endl;
    g_running = false;
}

// 格式化打印深度数据
void print_market_depth(const MarketDepthData* depth) {
    std::cout << "===== 市场深度数据 =====" << std::endl;
    std::cout << "币对ID: " << depth->pair_id 
              << " 时间戳: " << format_timestamp(depth->timestamp) << std::endl;
    
    std::cout << std::fixed << std::setprecision(2);
    
    std::cout << "卖单:" << std::endl;
    for (int i = 4; i >= 0; i--) {
        std::cout << "  " << std::setw(10) << depth->asks[i].price 
                  << " | " << std::setw(10) << depth->asks[i].quantity << std::endl;
    }
    
    std::cout << "---------------------" << std::endl;
    
    std::cout << "买单:" << std::endl;
    for (int i = 0; i < 5; i++) {
        std::cout << "  " << std::setw(10) << depth->bids[i].price 
                  << " | " << std::setw(10) << depth->bids[i].quantity << std::endl;
    }
    std::cout << std::endl;
}

// 格式化打印成交数据
void print_trade(const TradeData* trade) {
    std::cout << "===== 成交数据 =====" << std::endl;
    std::cout << "币对ID: " << trade->pair_id 
              << " 时间戳: " << format_timestamp(trade->timestamp) << std::endl;
    
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "成交ID: " << trade->trade_id
              << " 价格: " << trade->price
              << " 数量: " << trade->quantity
              << " 买方主动: " << (trade->is_buyer_maker ? "是" : "否") << std::endl;
    std::cout << std::endl;
}

// 格式化打印GARCH波动率数据
void print_garch_volatility(const GarchVolatilityData* volatility) {
    std::cout << "===== GARCH波动率数据 =====" << std::endl;
    std::cout << "币对ID: " << volatility->pair_id 
              << " 时间戳: " << format_timestamp(volatility->timestamp) << std::endl;
    
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "当前波动率: " << volatility->volatility
              << " 1小时预测: " << volatility->forecast_1h
              << " 24小时预测: " << volatility->forecast_24h << std::endl;
    std::cout << std::endl;
}

// 格式化打印K线数据
void print_candlestick(const CandlestickData* candle) {
    std::cout << "===== K线数据 =====" << std::endl;
    std::cout << "币对ID: " << candle->pair_id 
              << " 时间戳: " << format_timestamp(candle->timestamp) << std::endl;
    
    // 输出间隔描述
    std::string interval_desc;
    if (candle->interval == 60) interval_desc = "1分钟";
    else if (candle->interval == 300) interval_desc = "5分钟";
    else if (candle->interval == 900) interval_desc = "15分钟";
    else if (candle->interval == 3600) interval_desc = "1小时";
    else if (candle->interval == 86400) interval_desc = "1天";
    else interval_desc = std::to_string(candle->interval) + "秒";
    
    std::cout << "周期: " << interval_desc << std::endl;
    
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "开: " << candle->open
              << " 高: " << candle->high
              << " 低: " << candle->low
              << " 收: " << candle->close << std::endl;
    
    std::cout << "成交量: " << candle->volume
              << " 计价币种成交量: " << candle->quote_volume << std::endl;
    std::cout << std::endl;
}

// 格式化打印Ticker数据
void print_ticker(const TickerData* ticker) {
    std::cout << "===== Ticker数据 =====" << std::endl;
    std::cout << "币对ID: " << ticker->pair_id 
              << " 时间戳: " << format_timestamp(ticker->timestamp) << std::endl;
    
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "最新价: " << ticker->last_price
              << " 24小时变化: " << ticker->price_change_pct << "%" << std::endl;
    
    std::cout << "24小时最高: " << ticker->high_24h
              << " 24小时最低: " << ticker->low_24h
              << " 24小时成交量: " << ticker->volume_24h << std::endl;
    
    std::cout << "状态: " << (ticker->status == 1 ? "交易中" : "暂停") << std::endl;
    std::cout << std::endl;
}

int main(int argc, char** argv) {
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 默认连接参数
    std::string server_ip = "127.0.0.1";
    uint16_t server_port = 8888;
    bool use_shm = false;  // 使用TCP模式
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            server_ip = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--shm") {
            use_shm = true;
        } else if (arg == "--help") {
            std::cout << "使用方法: " << argv[0] << " [选项]" << std::endl;
            std::cout << "选项:" << std::endl;
            std::cout << "  --server IP    服务器IP地址 (默认: 127.0.0.1)" << std::endl;
            std::cout << "  --port PORT    服务器端口 (默认: 8888)" << std::endl;
            std::cout << "  --shm          使用共享内存 (默认: 使用TCP)" << std::endl;
            std::cout << "  --help         显示此帮助信息" << std::endl;
            return 0;
        }
    }
    
    std::cout << "加密货币市场数据客户端" << std::endl;
    std::cout << "连接到 " << server_ip << ":" << server_port;
    std::cout << " 使用" << (use_shm ? "共享内存" : "TCP") << std::endl;
    
    // 使用通用数据目录
    std::string data_dir = "/tmp/crypto_data";
    
    // 创建目录并设置权限
    system(("mkdir -p " + data_dir).c_str());
    system(("chmod 777 " + data_dir).c_str());
    
    std::cout << "使用数据目录: " << data_dir << std::endl;
    
    // 创建客户端实例
    MarketDataClient client(data_dir, "CryptoServer");
    
    // 注册回调函数
    client.RegisterMarketDepthCallback(print_market_depth);
    client.RegisterTradeCallback(print_trade);
    client.RegisterGarchVolatilityCallback(print_garch_volatility);
    client.RegisterCandlestickCallback(print_candlestick);
    client.RegisterTickerCallback(print_ticker);
    
    // 连接到服务器
    if (!client.ConnectToServer(use_shm, server_ip.c_str(), server_port)) {
        std::cerr << "连接失败!" << std::endl;
        return 1;
    }
    
    // 待订阅的币对
    std::vector<std::string> symbols = {"BTC/USDT", "ETH/USDT"};
    
    // 订阅所有数据类型
    uint8_t all_data_types = SUB_MARKET_DEPTH | SUB_TRADES | SUB_GARCH_VOLATILITY | 
                            SUB_CANDLESTICK | SUB_TICKER;
    
    if (!client.Subscribe(symbols, all_data_types)) {
        std::cerr << "订阅失败!" << std::endl;
    } else {
        std::cout << "已订阅 " << symbols.size() << " 个币对的数据" << std::endl;
    }
    
    // 运行客户端直到收到退出信号
    std::thread client_thread([&client]() {
        std::cout << "客户端启动，按Ctrl+C退出" << std::endl;
        client.Run();
    });
    
    // 等待退出信号
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 取消订阅
    client.Unsubscribe(symbols, all_data_types);
    
    // 停止客户端
    client.Close();
    client_thread.join();
    
    std::cout << "客户端已停止" << std::endl;
    return 0;
} 