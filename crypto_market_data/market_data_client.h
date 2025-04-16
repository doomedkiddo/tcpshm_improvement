#pragma once
#include "../tcpshm_client.h"
#include "common.h"
#include "market_data_types.h"
#include "timestamp.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>

namespace crypto {

// 前置声明
class MarketDataClient;

// 定义客户端类型
using TSClient = tcpshm::TcpShmClient<MarketDataClient, ClientConf>;

// 消息回调函数类型定义
using MarketDepthCallback = std::function<void(const MarketDepthData*)>;
using TradeCallback = std::function<void(const TradeData*)>;
using GarchVolatilityCallback = std::function<void(const GarchVolatilityData*)>;
using CandlestickCallback = std::function<void(const CandlestickData*)>;
using TickerCallback = std::function<void(const TickerData*)>;

// 币对信息类
class CurrencyPair {
public:
    CurrencyPair(uint32_t id, const std::string& symbol) 
        : id_(id), symbol_(symbol) {}
    
    uint32_t id() const { return id_; }
    const std::string& symbol() const { return symbol_; }
    
private:
    uint32_t id_;
    std::string symbol_;
};

// 加密货币市场数据客户端
class MarketDataClient : public TSClient
{
public:
    MarketDataClient(const std::string& ptcp_dir, const std::string& name)
        : TSClient(ptcp_dir, name), conn_(GetConnection()) {
        // 初始化币对映射表 (实际应用中可能从配置文件加载)
        InitializePairs();
    }
    
    // 连接到服务器
    bool ConnectToServer(bool use_shm, const char* server_ip, uint16_t server_port) {
        ClientConf::LoginData login_data;
        // 不再需要密钥和签名
        memset(login_data.api_key, 0, sizeof(login_data.api_key));
        memset(login_data.signature, 0, sizeof(login_data.signature));
        login_data.timestamp = now_ns();
        
        is_using_shm_ = use_shm;
        return Connect(use_shm, server_ip, server_port, login_data);
    }
    
    // 订阅市场数据 (可以订阅多个币对的多种数据类型)
    bool Subscribe(const std::vector<std::string>& symbols, uint8_t data_types) {
        if (symbols.empty() || data_types == 0) {
            return false;
        }
        
        // 将symbol转换为pair_id
        std::vector<uint32_t> pair_ids;
        for (const auto& symbol : symbols) {
            auto it = symbol_to_id_.find(symbol);
            if (it != symbol_to_id_.end()) {
                pair_ids.push_back(it->second);
            }
        }
        
        if (pair_ids.empty()) {
            return false;
        }
        
        // 创建订阅消息
        tcpshm::MsgHeader* header = conn_.Alloc(sizeof(SubscribeMsg));
        if (!header) {
            return false; // 发送队列已满
        }
        
        header->msg_type = SubscribeMsg::msg_type;
        SubscribeMsg* msg = reinterpret_cast<SubscribeMsg*>(header + 1);
        
        msg->data_types = data_types;
        msg->pair_count = std::min<uint8_t>(pair_ids.size(), sizeof(msg->pair_ids) / sizeof(msg->pair_ids[0]));
        
        for (uint8_t i = 0; i < msg->pair_count; i++) {
            msg->pair_ids[i] = pair_ids[i];
        }
        
        conn_.Push();
        return true;
    }
    
    // 取消订阅
    bool Unsubscribe(const std::vector<std::string>& symbols, uint8_t data_types) {
        if (symbols.empty() || data_types == 0) {
            return false;
        }
        
        // 将symbol转换为pair_id
        std::vector<uint32_t> pair_ids;
        for (const auto& symbol : symbols) {
            auto it = symbol_to_id_.find(symbol);
            if (it != symbol_to_id_.end()) {
                pair_ids.push_back(it->second);
            }
        }
        
        if (pair_ids.empty()) {
            return false;
        }
        
        // 创建取消订阅消息
        tcpshm::MsgHeader* header = conn_.Alloc(sizeof(UnsubscribeMsg));
        if (!header) {
            return false; // 发送队列已满
        }
        
        header->msg_type = UnsubscribeMsg::msg_type;
        UnsubscribeMsg* msg = reinterpret_cast<UnsubscribeMsg*>(header + 1);
        
        msg->data_types = data_types;
        msg->pair_count = std::min<uint8_t>(pair_ids.size(), sizeof(msg->pair_ids) / sizeof(msg->pair_ids[0]));
        
        for (uint8_t i = 0; i < msg->pair_count; i++) {
            msg->pair_ids[i] = pair_ids[i];
        }
        
        conn_.Push();
        return true;
    }
    
    // 注册回调函数
    void RegisterMarketDepthCallback(MarketDepthCallback callback) {
        market_depth_callback_ = std::move(callback);
    }
    
    void RegisterTradeCallback(TradeCallback callback) {
        trade_callback_ = std::move(callback);
    }
    
    void RegisterGarchVolatilityCallback(GarchVolatilityCallback callback) {
        garch_volatility_callback_ = std::move(callback);
    }
    
    void RegisterCandlestickCallback(CandlestickCallback callback) {
        candlestick_callback_ = std::move(callback);
    }
    
    void RegisterTickerCallback(TickerCallback callback) {
        ticker_callback_ = std::move(callback);
    }
    
    // 启动客户端循环
    void Run() {
        while (!conn_.IsClosed()) {
            // 轮询TCP和SHM
            PollTcp(now_ns());
            if (is_using_shm_) {
                PollShm();
            }
        }
    }
    
    // 停止客户端的方法
    void Close() {
        conn_.Close();
    }
    
    // 获取币对信息
    const CurrencyPair* GetPairBySymbol(const std::string& symbol) const {
        auto it = pairs_.find(symbol);
        if (it != pairs_.end()) {
            return &(it->second);
        }
        return nullptr;
    }
    
    const CurrencyPair* GetPairById(uint32_t id) const {
        auto it = id_to_symbol_.find(id);
        if (it != id_to_symbol_.end()) {
            return GetPairBySymbol(it->second);
        }
        return nullptr;
    }
    
    // 所有已知币对
    std::vector<CurrencyPair> GetAllPairs() const {
        std::vector<CurrencyPair> result;
        result.reserve(pairs_.size());
        
        for (const auto& pair : pairs_) {
            result.push_back(pair.second);
        }
        
        return result;
    }
    
private:
    // 初始化币对映射表 (实际应用中可能从配置文件加载)
    void InitializePairs() {
        // 添加一些示例币对
        AddPair(0, "BTC/USDT");
        AddPair(1, "ETH/USDT");
        AddPair(2, "BNB/USDT");
        AddPair(3, "SOL/USDT");
        AddPair(4, "XRP/USDT");
        // ... 可以添加更多
    }
    
    void AddPair(uint32_t id, const std::string& symbol) {
        pairs_.emplace(symbol, CurrencyPair(id, symbol));
        symbol_to_id_[symbol] = id;
        id_to_symbol_[id] = symbol;
    }
    
private:
    // TCPSHMClient 接口实现
    friend TSClient;
    
    // 连接错误回调
    void OnSystemError(const char* error_msg, int sys_errno) {
        std::cerr << "系统错误: " << error_msg 
                  << " 错误码: " << sys_errno 
                  << " (" << strerror(sys_errno) << ")" << std::endl;
    }
    
    // 登录被拒绝回调
    void OnLoginReject(const LoginRspMsg* login_rsp) {
        std::cerr << "登录被拒绝: " 
                  << "认证级别: " << static_cast<int>(login_rsp->user_data.auth_level)
                  << " 错误: " << login_rsp->user_data.error_msg << std::endl;
    }
    
    // 登录成功回调
    int64_t OnLoginSuccess(const LoginRspMsg* login_rsp) {
        std::cout << "登录成功! 认证级别: " 
                  << static_cast<int>(login_rsp->user_data.auth_level) << std::endl;
        return now_ns();
    }
    
    // 序列号不匹配回调
    void OnSeqNumberMismatch(uint32_t local_ack_seq,
                            uint32_t local_seq_start,
                            uint32_t local_seq_end,
                            uint32_t remote_ack_seq,
                            uint32_t remote_seq_start,
                            uint32_t remote_seq_end) {
        std::cerr << "序列号不匹配! 需要手动修复。" << std::endl
                  << "本地确认序列号: " << local_ack_seq 
                  << " 本地序列号范围: " << local_seq_start << "-" << local_seq_end
                  << " 远程确认序列号: " << remote_ack_seq
                  << " 远程序列号范围: " << remote_seq_start << "-" << remote_seq_end
                  << std::endl;
    }
    
    // 处理服务器消息
    void OnServerMsg(tcpshm::MsgHeader* header) {
        switch (header->msg_type) {
            case MarketDepthData::msg_type: {
                MarketDepthData* msg = reinterpret_cast<MarketDepthData*>(header + 1);
                if (market_depth_callback_) {
                    market_depth_callback_(msg);
                }
                break;
            }
            case TradeData::msg_type: {
                TradeData* msg = reinterpret_cast<TradeData*>(header + 1);
                if (trade_callback_) {
                    trade_callback_(msg);
                }
                break;
            }
            case GarchVolatilityData::msg_type: {
                GarchVolatilityData* msg = reinterpret_cast<GarchVolatilityData*>(header + 1);
                if (garch_volatility_callback_) {
                    garch_volatility_callback_(msg);
                }
                break;
            }
            case CandlestickData::msg_type: {
                CandlestickData* msg = reinterpret_cast<CandlestickData*>(header + 1);
                if (candlestick_callback_) {
                    candlestick_callback_(msg);
                }
                break;
            }
            case TickerData::msg_type: {
                TickerData* msg = reinterpret_cast<TickerData*>(header + 1);
                if (ticker_callback_) {
                    ticker_callback_(msg);
                }
                break;
            }
            case MSG_RESPONSE: {
                ResponseMsg* msg = reinterpret_cast<ResponseMsg*>(header + 1);
                HandleResponseMsg(msg);
                break;
            }
            // 处理批量消息类型
            case BatchMarketDepth::msg_type: {
                HandleBatchMarketDepth(header);
                break;
            }
            case BatchTrade::msg_type: {
                HandleBatchTrade(header);
                break;
            }
            // 可以添加其他批量消息类型的处理
            default:
                std::cerr << "收到未知消息类型: " << header->msg_type << std::endl;
                break;
        }
        
        // 消费掉这条消息
        conn_.Pop();
    }
    
    // 处理响应消息
    void HandleResponseMsg(ResponseMsg* msg) {
        std::cout << "收到响应: 请求类型=" << msg->req_msg_type
                  << " 状态=" << static_cast<int>(msg->status)
                  << " 消息=\"" << msg->message << "\"" << std::endl;
    }
    
    // 处理批量市场深度数据
    void HandleBatchMarketDepth(tcpshm::MsgHeader* header) {
        if (!market_depth_callback_) return;
        
        BatchMarketDepth* batch = reinterpret_cast<BatchMarketDepth*>(header + 1);
        MarketDepthData* entries = batch->entries;
        
        for (uint8_t i = 0; i < batch->count; i++) {
            market_depth_callback_(&entries[i]);
        }
    }
    
    // 处理批量成交数据
    void HandleBatchTrade(tcpshm::MsgHeader* header) {
        if (!trade_callback_) return;
        
        BatchTrade* batch = reinterpret_cast<BatchTrade*>(header + 1);
        TradeData* entries = batch->entries;
        
        for (uint8_t i = 0; i < batch->count; i++) {
            trade_callback_(&entries[i]);
        }
    }
    
    // 断开连接回调
    void OnDisconnected(const char* reason, int sys_errno) {
        std::cerr << "连接断开: " << reason;
        if (sys_errno != 0) {
            std::cerr << " 错误码: " << sys_errno 
                      << " (" << strerror(sys_errno) << ")";
        }
        std::cerr << std::endl;
    }
    
private:
    // 数据成员
    tcpshm::TcpShmConnection<ClientConf>& conn_;
    bool is_using_shm_ = false;
    
    // 回调函数
    MarketDepthCallback market_depth_callback_;
    TradeCallback trade_callback_;
    GarchVolatilityCallback garch_volatility_callback_;
    CandlestickCallback candlestick_callback_;
    TickerCallback ticker_callback_;
    
    // 币对映射
    std::unordered_map<std::string, CurrencyPair> pairs_;
    std::unordered_map<std::string, uint32_t> symbol_to_id_;
    std::unordered_map<uint32_t, std::string> id_to_symbol_;
};

} // namespace crypto