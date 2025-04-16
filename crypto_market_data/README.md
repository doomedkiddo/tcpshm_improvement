# 加密货币市场数据传输示例

本项目展示了如何使用TCPSHM框架传输多种类型的加密货币市场数据，确保数据传输的可靠性和高性能。

## 功能特点

- 支持五种不同类型的市场数据：
  1. 五档深度行情数据 (Market Depth)
  2. 成交数据 (Trades)
  3. GARCH波动率数据 (Volatility) 
  4. K线数据 (Candlestick)
  5. 币对状态数据 (Ticker)

- 支持多达120个币对
- 支持TCP和共享内存(SHM)两种传输方式
- 确保数据可靠性和不丢失
- 非阻塞设计，高性能
- 支持按需订阅数据

## 项目结构

```
├── market_data_types.h     # 数据类型定义
├── common.h                # 公共配置和定义
├── timestamp.h             # 时间戳工具
├── market_data_client.h    # 客户端实现
├── client_example.cpp      # 客户端示例程序
├── build.sh                # 构建脚本
└── CMakeLists.txt          # 构建文件
```

## 编译与运行

### 使用构建脚本

项目提供了便捷的构建脚本，可以快速编译和运行示例：

```bash
# 编译项目
./build.sh

# 清理后重新编译
./build.sh --clean

# 以调试模式编译
./build.sh --debug

# 编译后立即运行示例
./build.sh --run

# 编译后以共享内存模式运行示例
./build.sh --run-shm

# 查看构建脚本帮助
./build.sh --help
```

### 手动编译

如果不使用构建脚本，也可以手动编译：

```bash
mkdir -p build && cd build
cmake ..
make
```

### 运行客户端示例

```bash
./client_example [选项]
```

### 命令行选项

- `--server IP`：服务器IP地址 (默认: 127.0.0.1)
- `--port PORT`：服务器端口 (默认: 8888)
- `--shm`：使用共享内存 (默认: 使用TCP)
- `--key KEY`：API密钥 (默认: demo_key)
- `--sig SIG`：签名 (默认: demo_signature)
- `--help`：显示帮助信息

## 使用示例

### 数据类型

本示例实现了以下5种市场数据类型：

```cpp
// 1. 五档深度行情数据
struct MarketDepthData {
    // 买卖盘五档价格数量信息
};

// 2. 成交数据
struct TradeData {
    // 成交价格、数量、方向等信息
};

// 3. GARCH波动率数据
struct GarchVolatilityData {
    // 当前波动率和预测值
};

// 4. K线数据
struct CandlestickData {
    // 开高低收量等信息
};

// 5. 币对状态数据
struct TickerData {
    // 最新价格、24小时涨跌幅等概览信息
};
```

### 客户端使用

```cpp
// 创建客户端
MarketDataClient client("./client", "CryptoClient");

// 注册回调
client.RegisterMarketDepthCallback([](const MarketDepthData* data) {
    // 处理深度数据
});

// 连接服务器
client.ConnectToServer(use_shm, server_ip, server_port, api_key, signature);

// 订阅数据
std::vector<std::string> symbols = {"BTC/USDT", "ETH/USDT"};
client.Subscribe(symbols, SUB_MARKET_DEPTH | SUB_TRADES);

// 运行客户端
client.Run();

// 取消订阅和停止
client.Unsubscribe(symbols, SUB_MARKET_DEPTH | SUB_TRADES);
client.Close();
```

## 框架优势

- **可靠性**: 通过序列号和确认机制确保消息不会丢失，即使断线或崩溃也能恢复
- **高性能**: 共享内存模式比TCP localhost快20倍
- **兼容性**: TCP和SHM提供相同的API接口，使用透明
- **灵活性**: 支持批量消息处理，优化网络资源

## 注意事项

- 请确保客户端和服务器配置匹配，特别是共享内存队列大小
- 处理消息的回调函数应尽量简短，避免阻塞消息接收线程
- 高频交易场景建议使用共享内存模式以获得最低延迟 