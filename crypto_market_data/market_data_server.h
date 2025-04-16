#pragma once
#include "../tcpshm_server.h"
#include "common.h"
#include "market_data_types.h"
#include "timestamp.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>
#include <random>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>  // for std::find

namespace crypto {

// 前置声明
class MarketDataServer;

// 定义服务器类型
using TSServer = tcpshm::TcpShmServer<MarketDataServer, ServerConf>;

// 加密货币市场数据服务器
class MarketDataServer : public TSServer
{
public:
    MarketDataServer(const std::string& name, const std::string& ptcp_dir)
        : TSServer(name, ptcp_dir), 
          server_port_(0),
          random_engine_(std::random_device()()) {
        // 初始化币对
        InitializePairs();
        
        // 初始化各币对的初始价格
        InitializePrices();
    }
    
    // 设置服务器端口
    void SetPort(uint16_t port) {
        server_port_ = port;
    }
    
    // 获取服务器端口
    uint16_t GetPort() const {
        return server_port_;
    }
    
    // 启动服务器
    bool Start(const char* listen_ip = "0.0.0.0") {
        bool success = TSServer::Start(listen_ip, server_port_);
        if (success) {
            std::cout << "市场数据服务器启动于 " << listen_ip << ":" << server_port_ << std::endl;
            is_running_ = true;
            
            // 创建模拟数据线程
            market_data_thread_ = std::thread(&MarketDataServer::GenerateMarketData, this);
        }
        else {
            std::cerr << "启动服务器失败!" << std::endl;
        }
        return success;
    }
    
    // 停止服务器
    void Stop() {
        is_running_ = false;
        if (market_data_thread_.joinable()) {
            market_data_thread_.join();
        }
        TSServer::Stop();
    }
    
    // 轮询服务器 (需要在主线程中定期调用)
    void Poll() {
        int64_t now = now_ns();
        
        // 轮询控制
        PollCtl(now);
        
        // 轮询所有TCP组
        for (uint32_t i = 0; i < ServerConf::MaxTcpGrps; i++) {
            PollTcp(now, i);
        }
        
        // 轮询所有SHM组
        for (uint32_t i = 0; i < ServerConf::MaxShmGrps; i++) {
            PollShm(i);
        }
    }
    
    // 所有已知币对
    std::vector<PairMapping> GetAllPairs() const {
        std::vector<PairMapping> result;
        result.reserve(pairs_.size());
        
        for (const auto& pair : pairs_) {
            PairMapping mapping;
            mapping.pair_id = pair.second.first;
            strncpy(mapping.symbol, pair.first.c_str(), sizeof(mapping.symbol) - 1);
            mapping.symbol[sizeof(mapping.symbol) - 1] = '\0';
            result.push_back(mapping);
        }
        
        return result;
    }
    
private:
    // 初始化币对
    void InitializePairs() {
        // 添加一些示例币对
        AddPair(0, "BTC/USDT");
        AddPair(1, "ETH/USDT");
        AddPair(2, "BNB/USDT");
        AddPair(3, "SOL/USDT");
        AddPair(4, "XRP/USDT");
        // 可以添加更多币对
    }
    
    // 添加币对
    void AddPair(uint32_t id, const std::string& symbol) {
        pairs_[symbol] = std::make_pair(id, symbol);
        id_to_symbol_[id] = symbol;
    }
    
    // 初始化价格数据
    void InitializePrices() {
        // 示例初始价格
        pair_prices_[0] = 27500.0;  // BTC/USDT
        pair_prices_[1] = 1850.0;   // ETH/USDT
        pair_prices_[2] = 320.0;    // BNB/USDT
        pair_prices_[3] = 75.0;     // SOL/USDT
        pair_prices_[4] = 0.51;     // XRP/USDT
        
        // 初始化成交量和24小时高低价
        for (const auto& pair : pairs_) {
            uint32_t id = pair.second.first;
            double base_price = pair_prices_[id];
            
            // 初始化波动率数据 (每个币对不同)
            volatility_data_[id].volatility = 0.01 + (id * 0.002);
            volatility_data_[id].forecast_1h = volatility_data_[id].volatility * 0.9;
            volatility_data_[id].forecast_24h = volatility_data_[id].volatility * 0.8;
            
            // 初始化24小时Ticker数据
            ticker_data_[id].high_24h = base_price * (1.0 + 0.01 * (id + 1));
            ticker_data_[id].low_24h = base_price * (1.0 - 0.01 * (id + 1));
            ticker_data_[id].volume_24h = 1000.0 * (id + 1);
            ticker_data_[id].last_price = base_price;
            ticker_data_[id].price_change_pct = (id % 2 == 0) ? 2.5 : -1.8;
            ticker_data_[id].status = 1; // 交易中
            
            // 初始化K线数据
            for (uint32_t interval : {60, 300, 900, 3600, 86400}) {
                CandlestickData& candle = candlestick_data_[id][interval];
                candle.interval = interval;
                candle.open = base_price * 0.99;
                candle.high = base_price * 1.01;
                candle.low = base_price * 0.98;
                candle.close = base_price;
                candle.volume = 100.0 * (id + 1);
                candle.quote_volume = candle.volume * base_price;
            }
            
            // 初始化深度数据
            InitializeMarketDepth(id);
        }
    }
    
    // 初始化市场深度数据
    void InitializeMarketDepth(uint32_t pair_id) {
        double base_price = pair_prices_[pair_id];
        double spread = base_price * 0.0005; // 0.05% 价差
        
        // 创建买卖盘
        for (int i = 0; i < 5; i++) {
            // 买盘 (递减)
            market_depth_[pair_id].bids[i].price = base_price - spread - (i * base_price * 0.0010);
            market_depth_[pair_id].bids[i].quantity = 10.0 * (5 - i) + (random_engine_() % 10);
            
            // 卖盘 (递增)
            market_depth_[pair_id].asks[i].price = base_price + spread + (i * base_price * 0.0010);
            market_depth_[pair_id].asks[i].quantity = 8.0 * (5 - i) + (random_engine_() % 10);
        }
    }
    
    // 模拟市场数据生成线程
    void GenerateMarketData() {
        int64_t trade_id = 1;
        
        while (is_running_) {
            int64_t current_time = now_ns();
            
            // 随机选择一个或多个币对更新
            int num_updates = 1 + (random_engine_() % 3); // 随机1-3个更新
            
            for (int i = 0; i < num_updates; i++) {
                uint32_t pair_id = random_engine_() % pairs_.size();
                
                // 更新价格 (随机波动)
                std::normal_distribution<double> price_change(0, volatility_data_[pair_id].volatility * pair_prices_[pair_id] * 0.01);
                double delta = price_change(random_engine_);
                double old_price = pair_prices_[pair_id];
                double new_price = old_price + delta;
                pair_prices_[pair_id] = new_price;
                
                // 创建交易数据
                TradeData trade;
                trade.pair_id = pair_id;
                trade.timestamp = current_time;
                trade.trade_id = trade_id++;
                trade.price = new_price;
                trade.quantity = 0.1 + (random_engine_() % 100) / 10.0;
                trade.is_buyer_maker = (random_engine_() % 2) == 0;
                
                // 更新市场深度
                UpdateMarketDepth(pair_id, new_price);
                
                // 更新K线数据
                UpdateCandlestick(pair_id, trade.price, trade.quantity, current_time);
                
                // 更新Ticker数据
                UpdateTicker(pair_id, trade.price, trade.quantity);
                
                // 每10秒更新一次波动率
                if (current_time - last_volatility_update_ > 10 * 1000000000LL) {
                    UpdateVolatility();
                    last_volatility_update_ = current_time;
                }
                
                // 向订阅者广播数据
                BroadcastData(pair_id, current_time);
            }
            
            // 等待一个短暂的时间间隔
            std::this_thread::sleep_for(std::chrono::milliseconds(100 + (random_engine_() % 150)));
        }
    }
    
    // 更新市场深度
    void UpdateMarketDepth(uint32_t pair_id, double price) {
        double spread = price * 0.0003 + (random_engine_() % 100) / 10000.0 * price;
        
        // 更新所有价格水平
        for (int i = 0; i < 5; i++) {
            // 根据新价格调整买卖盘
            market_depth_[pair_id].bids[i].price = price - spread - (i * price * 0.0008);
            market_depth_[pair_id].asks[i].price = price + spread + (i * price * 0.0008);
            
            // 随机波动数量
            if (random_engine_() % 3 == 0) { // 1/3几率更新数量
                market_depth_[pair_id].bids[i].quantity += ((random_engine_() % 100) / 50.0 - 1.0);
                if (market_depth_[pair_id].bids[i].quantity < 0.1) 
                    market_depth_[pair_id].bids[i].quantity = 0.1;
                
                market_depth_[pair_id].asks[i].quantity += ((random_engine_() % 100) / 50.0 - 1.0);
                if (market_depth_[pair_id].asks[i].quantity < 0.1) 
                    market_depth_[pair_id].asks[i].quantity = 0.1;
            }
        }
        
        // 更新时间戳
        market_depth_[pair_id].pair_id = pair_id;
        market_depth_[pair_id].timestamp = now_ns();
    }
    
    // 更新K线数据
    void UpdateCandlestick(uint32_t pair_id, double price, double quantity, int64_t timestamp) {
        for (auto& [interval, candle] : candlestick_data_[pair_id]) {
            // 检查是否需要创建新的K线
            int64_t interval_ns = interval * 1000000000LL;
            int64_t candle_start = (timestamp / interval_ns) * interval_ns;
            
            if (candle_start > candle.timestamp) {
                // 创建新K线
                candle.timestamp = candle_start;
                candle.open = price;
                candle.high = price;
                candle.low = price;
                candle.close = price;
                candle.volume = quantity;
                candle.quote_volume = quantity * price;
            }
            else {
                // 更新现有K线
                candle.high = std::max(candle.high, price);
                candle.low = std::min(candle.low, price);
                candle.close = price;
                candle.volume += quantity;
                candle.quote_volume += quantity * price;
            }
        }
    }
    
    // 更新Ticker数据
    void UpdateTicker(uint32_t pair_id, double price, double quantity) {
        ticker_data_[pair_id].pair_id = pair_id;
        ticker_data_[pair_id].timestamp = now_ns();
        ticker_data_[pair_id].last_price = price;
        
        // 更新24小时高低价
        ticker_data_[pair_id].high_24h = std::max(ticker_data_[pair_id].high_24h, price);
        ticker_data_[pair_id].low_24h = std::min(ticker_data_[pair_id].low_24h, price);
        
        // 更新24小时成交量
        ticker_data_[pair_id].volume_24h += quantity;
        
        // 随机更新24小时价格变化
        if (random_engine_() % 20 == 0) { // 偶尔更新
            ticker_data_[pair_id].price_change_pct = ((random_engine_() % 1000) / 100.0) - 5.0;
        }
    }
    
    // 更新波动率数据
    void UpdateVolatility() {
        for (auto& [pair_id, vol_data] : volatility_data_) {
            // 随机调整波动率
            double change = ((random_engine_() % 100) / 1000.0) - 0.05;
            vol_data.volatility = std::max(0.005, vol_data.volatility + change);
            vol_data.forecast_1h = vol_data.volatility * (0.8 + (random_engine_() % 40) / 100.0);
            vol_data.forecast_24h = vol_data.volatility * (0.6 + (random_engine_() % 60) / 100.0);
            
            vol_data.pair_id = pair_id;
            vol_data.timestamp = now_ns();
        }
    }
    
    // 向所有订阅者广播市场数据
    void BroadcastData(uint32_t pair_id, int64_t timestamp) {
        // 使用ForEachConn来遍历所有连接并广播数据
        ForEachConn([&](Connection& conn) {
            if (conn.IsClosed()) return true;
            
            // 检查客户端是否订阅了这个币对
            auto& user_data = conn.user_data;
            uint32_t pair_word_idx = pair_id / 32;
            uint32_t pair_bit_idx = pair_id % 32;
            
            if (pair_word_idx < 4 && (user_data.subscribed_pairs[pair_word_idx] & (1 << pair_bit_idx))) {
                // 检查订阅的数据类型并发送
                SendMarketDataToClient(conn, pair_id, user_data.subscription_types, timestamp);
            }
            
            return true; // 继续遍历
        });
    }
    
    // 向客户端发送市场数据
    void SendMarketDataToClient(Connection& conn, uint32_t pair_id, 
                               uint8_t subscription_types, int64_t timestamp) {
        // 市场深度数据
        if (subscription_types & SUB_MARKET_DEPTH) {
            auto& depth = market_depth_[pair_id];
            depth.timestamp = timestamp;
            
            tcpshm::MsgHeader* header = conn.Alloc(sizeof(MarketDepthData));
            if (header) {
                header->msg_type = MarketDepthData::msg_type;
                MarketDepthData* msg = reinterpret_cast<MarketDepthData*>(header + 1);
                *msg = depth;
                conn.Push();
            }
        }
        
        // 交易数据 (只在价格变化时发送)
        if (subscription_types & SUB_TRADES && random_engine_() % 4 == 0) {
            TradeData trade;
            trade.pair_id = pair_id;
            trade.timestamp = timestamp;
            trade.trade_id = trade_counter_++;
            trade.price = pair_prices_[pair_id];
            trade.quantity = 0.1 + (random_engine_() % 100) / 10.0;
            trade.is_buyer_maker = (random_engine_() % 2) == 0;
            
            tcpshm::MsgHeader* header = conn.Alloc(sizeof(TradeData));
            if (header) {
                header->msg_type = TradeData::msg_type;
                TradeData* msg = reinterpret_cast<TradeData*>(header + 1);
                *msg = trade;
                conn.Push();
            }
        }
        
        // 波动率数据 (偶尔发送)
        if (subscription_types & SUB_GARCH_VOLATILITY && random_engine_() % 10 == 0) {
            auto& vol_data = volatility_data_[pair_id];
            vol_data.timestamp = timestamp;
            
            tcpshm::MsgHeader* header = conn.Alloc(sizeof(GarchVolatilityData));
            if (header) {
                header->msg_type = GarchVolatilityData::msg_type;
                GarchVolatilityData* msg = reinterpret_cast<GarchVolatilityData*>(header + 1);
                *msg = vol_data;
                conn.Push();
            }
        }
        
        // K线数据 (偶尔发送)
        if (subscription_types & SUB_CANDLESTICK && random_engine_() % 8 == 0) {
            // 随机选择一个周期
            uint32_t intervals[] = {60, 300, 900, 3600, 86400};
            uint32_t selected_interval = intervals[random_engine_() % 5];
            
            auto& candle = candlestick_data_[pair_id][selected_interval];
            candle.timestamp = timestamp - (timestamp % (selected_interval * 1000000000LL));
            
            tcpshm::MsgHeader* header = conn.Alloc(sizeof(CandlestickData));
            if (header) {
                header->msg_type = CandlestickData::msg_type;
                CandlestickData* msg = reinterpret_cast<CandlestickData*>(header + 1);
                *msg = candle;
                conn.Push();
            }
        }
        
        // Ticker数据 (偶尔发送)
        if (subscription_types & SUB_TICKER && random_engine_() % 6 == 0) {
            auto& ticker = ticker_data_[pair_id];
            ticker.timestamp = timestamp;
            
            tcpshm::MsgHeader* header = conn.Alloc(sizeof(TickerData));
            if (header) {
                header->msg_type = TickerData::msg_type;
                TickerData* msg = reinterpret_cast<TickerData*>(header + 1);
                *msg = ticker;
                conn.Push();
            }
        }
    }
    
private:
    // ForEachConn iterates over all connections and applies the callback function
    template<typename Callback>
    void ForEachConn(Callback callback) {
        // 使用我们自己维护的活动连接列表
        for (auto* conn_ptr : active_connections_) {
            if (!callback(*conn_ptr)) break;
        }
    }
    
    // TCPSHMServer 接口实现
    friend TSServer;
    
    // 系统错误回调
    void OnSystemError(const char* error_msg, int sys_errno) {
        std::cerr << "系统错误: " << error_msg 
                  << " 错误码: " << sys_errno
                  << " (" << strerror(sys_errno) << ")" << std::endl;
    }
    
    // 分配连接到连接组
    uint32_t OnNewConnection(const struct sockaddr_in& addr, const TSServer::LoginMsg* login_msg, TSServer::LoginRspMsg* login_rsp) {
        std::string remote_ip = inet_ntoa(addr.sin_addr);
        uint16_t remote_port = ntohs(addr.sin_port);
        bool is_shm = login_msg->use_shm;
        
        std::cout << "新连接来自 " << remote_ip << ":" << remote_port
                  << " 使用" << (is_shm ? "共享内存" : "TCP") << std::endl;
                   
        // 选择负载最轻的组
        uint32_t num_groups = is_shm ? ServerConf::MaxShmGrps : ServerConf::MaxTcpGrps;
        
        // 简单地随机选择一个组
        return random_engine_() % num_groups;
    }

    // 文件操作错误回调
    void OnClientFileError(Connection& conn, const char* error_msg, int sys_errno) {
        std::cerr << "客户端文件错误: " << conn.GetRemoteName() 
                  << " 错误: " << error_msg 
                  << " 错误码: " << sys_errno << std::endl;
    }
    
    // 序列号不匹配回调
    void OnSeqNumberMismatch(Connection& conn, 
                            uint32_t local_ack_seq, uint32_t local_seq_start, uint32_t local_seq_end,
                            uint32_t remote_ack_seq, uint32_t remote_seq_start, uint32_t remote_seq_end) {
        std::cerr << "序列号不匹配: " << conn.GetRemoteName() << std::endl;
        std::cerr << "本地序列: ack=" << local_ack_seq 
                  << " start=" << local_seq_start 
                  << " end=" << local_seq_end << std::endl;
        std::cerr << "远程序列: ack=" << remote_ack_seq 
                  << " start=" << remote_seq_start 
                  << " end=" << remote_seq_end << std::endl;
    }
    
    // 客户端登录成功回调
    void OnClientLogon(const struct sockaddr_in& addr, Connection& conn) {
        std::string remote_ip = inet_ntoa(addr.sin_addr);
        uint16_t remote_port = ntohs(addr.sin_port);
        
        std::cout << "客户端登录成功: " << remote_ip << ":" << remote_port
                  << " 客户端名称: " << conn.GetRemoteName() << std::endl;
        
        // 初始化连接用户数据
        auto& user_data = conn.user_data;
        memset(user_data.subscribed_pairs, 0, sizeof(user_data.subscribed_pairs));
        user_data.subscription_types = 0;
        user_data.last_activity_time = now_ns();
        user_data.msg_count = 0;
        
        // 添加到活动连接列表
        active_connections_.push_back(&conn);
    }
    
    // 连接关闭回调
    void OnClientDisconnected(Connection& conn, const char* reason, int) {
        std::cout << "客户端断开连接: " << conn.GetRemoteName() 
                  << " 原因: " << reason << std::endl;
        
        // 从活动连接列表中移除
        auto it = std::find(active_connections_.begin(), active_connections_.end(), &conn);
        if (it != active_connections_.end()) {
            active_connections_.erase(it);
        }
    }
    
    // 处理来自客户端的消息
    void OnClientMsg(Connection& conn, const tcpshm::MsgHeader* header) {
        // 更新连接活动时间和消息计数
        auto& user_data = conn.user_data;
        user_data.last_activity_time = now_ns();
        user_data.msg_count++;
        
        const void* msg = header + 1; // 消息内容紧跟在头部之后
        
        // 处理不同类型的消息
        if (header->msg_type == MSG_SUBSCRIBE) {
            HandleSubscribeMsg(conn, static_cast<const SubscribeMsg*>(msg));
        }
        else if (header->msg_type == MSG_UNSUBSCRIBE) {
            HandleUnsubscribeMsg(conn, static_cast<const UnsubscribeMsg*>(msg));
        }
        
        // 消息处理完成后，弹出消息
        conn.Pop();
    }
    
    // 处理订阅请求
    void HandleSubscribeMsg(Connection& conn, const SubscribeMsg* msg) {
        auto& user_data = conn.user_data;
        
        // 更新订阅类型
        user_data.subscription_types |= msg->data_types;
        
        // 添加订阅的币对
        for (uint8_t i = 0; i < msg->pair_count; i++) {
            uint32_t pair_id = msg->pair_ids[i];
            uint32_t word_idx = pair_id / 32;
            uint32_t bit_idx = pair_id % 32;
            
            if (word_idx < 4) {
                user_data.subscribed_pairs[word_idx] |= (1 << bit_idx);
            }
        }
        
        // 发送订阅确认
        SendResponseMsg(conn, MSG_SUBSCRIBE, 0, "订阅成功");
        
        std::cout << "客户端 " << conn.GetRemoteName()
                  << " 订阅了 " << static_cast<int>(msg->pair_count) << " 个币对的数据" << std::endl;
    }
    
    // 处理取消订阅请求
    void HandleUnsubscribeMsg(Connection& conn, const UnsubscribeMsg* msg) {
        auto& user_data = conn.user_data;
        
        // 如果是取消所有类型，直接清除对应币对的所有订阅
        if (msg->data_types == (SUB_MARKET_DEPTH | SUB_TRADES | SUB_GARCH_VOLATILITY | 
                              SUB_CANDLESTICK | SUB_TICKER)) {
            for (uint8_t i = 0; i < msg->pair_count; i++) {
                uint32_t pair_id = msg->pair_ids[i];
                uint32_t word_idx = pair_id / 32;
                uint32_t bit_idx = pair_id % 32;
                
                if (word_idx < 4) {
                    user_data.subscribed_pairs[word_idx] &= ~(1 << bit_idx);
                }
            }
        }
        
        // 检查是否还有任何币对订阅了指定的数据类型
        bool any_remaining = false;
        for (int i = 0; i < 4; i++) {
            if (user_data.subscribed_pairs[i] != 0) {
                any_remaining = true;
                break;
            }
        }
        
        // 如果没有任何币对订阅了这些类型，可以清除订阅类型标志
        if (!any_remaining) {
            user_data.subscription_types &= ~msg->data_types;
        }
        
        // 发送取消订阅确认
        SendResponseMsg(conn, MSG_UNSUBSCRIBE, 0, "取消订阅成功");
        
        std::cout << "客户端 " << conn.GetRemoteName()
                  << " 取消订阅了 " << static_cast<int>(msg->pair_count) << " 个币对的数据" << std::endl;
    }
    
    // 发送响应消息
    void SendResponseMsg(Connection& conn, uint16_t req_type, uint8_t status, const char* message) {
        tcpshm::MsgHeader* header = conn.Alloc(sizeof(ResponseMsg));
        if (header) {
            header->msg_type = ResponseMsg::msg_type;
            ResponseMsg* rsp = reinterpret_cast<ResponseMsg*>(header + 1);
            
            rsp->req_msg_type = req_type;
            rsp->status = status;
            strncpy(rsp->message, message, sizeof(rsp->message) - 1);
            rsp->message[sizeof(rsp->message) - 1] = '\0';
            
            conn.Push();
        }
    }
    
private:
    // 状态和数据
    uint16_t server_port_;                   // 服务器端口
    std::atomic<bool> is_running_{false};    // 运行标志
    std::thread market_data_thread_;         // 市场数据生成线程
    std::mt19937 random_engine_;             // 随机数生成器
    int64_t trade_counter_{1};               // 交易ID计数器
    int64_t last_volatility_update_{0};      // 上次波动率更新时间
    
    // 币对映射表
    std::unordered_map<std::string, std::pair<uint32_t, std::string>> pairs_;
    std::unordered_map<uint32_t, std::string> id_to_symbol_;
    
    // 市场数据
    std::unordered_map<uint32_t, double> pair_prices_;
    std::unordered_map<uint32_t, MarketDepthData> market_depth_;
    std::unordered_map<uint32_t, GarchVolatilityData> volatility_data_;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, CandlestickData>> candlestick_data_;
    std::unordered_map<uint32_t, TickerData> ticker_data_;
    
    // 维护活动连接列表
    std::vector<Connection*> active_connections_;
};

} // namespace crypto 