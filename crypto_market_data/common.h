#pragma once
#include <cstdint>

// 客户端和服务器共享的配置参数
struct CryptoMarketConf
{
    // 连接配置
    static constexpr uint32_t NameSize = 16;
    static constexpr uint32_t ShmQueueSize = 4 * 1024 * 1024; // 4MB 共享内存队列
    static constexpr bool ToLittleEndian = true; // 使用小端序

    // TCP 配置
    static constexpr uint32_t TcpQueueSize = 8000;       // TCP队列大小
    static constexpr uint32_t TcpRecvBufInitSize = 4000; // TCP接收缓冲区初始大小
    static constexpr uint32_t TcpRecvBufMaxSize = 16000; // TCP接收缓冲区最大大小
    static constexpr bool TcpNoDelay = true;             // 启用TCP_NODELAY选项

    // 时间配置（纳秒单位）
    static constexpr int64_t NanoInSecond = 1000000000LL;
    static constexpr int64_t ConnectionTimeout = 10 * NanoInSecond;  // 连接超时
    static constexpr int64_t HeartBeatInverval = 1 * NanoInSecond;   // 心跳间隔

    // 登录数据类型
    struct LoginData {
        char api_key[32];
        char signature[64];
        uint64_t timestamp;
    };

    struct LoginRspData {
        uint8_t auth_level; // 认证级别：0=拒绝, 1=只读, 2=交易, 3=管理
        char error_msg[64]; // 错误消息，如果有
    };

    // 用户自定义数据类型
    using LoginUserData = LoginData;
    using LoginRspUserData = LoginRspData;
    
    // 连接用户数据，可存储连接相关的状态
    struct ConnUserData {
        uint32_t subscribed_pairs[4]; // 支持订阅多达128个币对 (4*32=128位)
        uint8_t subscription_types;   // 订阅的数据类型位掩码
        uint64_t last_activity_time;  // 上次活动时间
        uint32_t msg_count;           // 消息计数
    };
    using ConnectionUserData = ConnUserData;
};

// 服务器特定配置
struct ServerConf : public CryptoMarketConf
{
    // 服务器连接配置
    static constexpr uint32_t MaxNewConnections = 10;    // 最大未登录连接数
    static constexpr uint32_t MaxShmConnsPerGrp = 16;    // 每组最大共享内存连接数
    static constexpr uint32_t MaxShmGrps = 2;            // 共享内存连接组数
    static constexpr uint32_t MaxTcpConnsPerGrp = 32;    // 每组最大TCP连接数
    static constexpr uint32_t MaxTcpGrps = 4;            // TCP连接组数
    static constexpr int64_t NewConnectionTimeout = 5 * NanoInSecond; // 新连接超时
};

// 客户端特定配置
struct ClientConf : public CryptoMarketConf
{
    // 客户端特定配置可以在这里添加
};

// 加密货币数据类型的订阅标志
enum SubscriptionFlags {
    SUB_MARKET_DEPTH = 1 << 0,
    SUB_TRADES = 1 << 1,
    SUB_GARCH_VOLATILITY = 1 << 2,
    SUB_CANDLESTICK = 1 << 3,
    SUB_TICKER = 1 << 4
};

// 控制消息类型
enum ControlMsgType {
    // 预留0用于系统
    MSG_SUBSCRIBE = 200,      // 订阅请求
    MSG_UNSUBSCRIBE = 201,    // 取消订阅请求
    MSG_RESPONSE = 202,       // 通用响应消息
    MSG_HEARTBEAT = 203       // 心跳消息
};

// 订阅请求消息
struct SubscribeMsg {
    static constexpr uint16_t msg_type = MSG_SUBSCRIBE;
    
    uint8_t data_types;     // 数据类型位掩码 (使用SubscriptionFlags)
    uint8_t pair_count;     // 币对数量
    uint32_t pair_ids[30];  // 要订阅的币对ID列表 (最多30个)
};

// 取消订阅请求消息
struct UnsubscribeMsg {
    static constexpr uint16_t msg_type = MSG_UNSUBSCRIBE;
    
    uint8_t data_types;     // 数据类型位掩码 (使用SubscriptionFlags)
    uint8_t pair_count;     // 币对数量
    uint32_t pair_ids[30];  // 要取消订阅的币对ID列表 (最多30个)
};

// 通用响应消息
struct ResponseMsg {
    static constexpr uint16_t msg_type = MSG_RESPONSE;
    
    uint16_t req_msg_type;  // 请求消息类型
    uint8_t status;         // 状态码: 0=成功, 非0=失败
    char message[64];       // 状态信息
}; 