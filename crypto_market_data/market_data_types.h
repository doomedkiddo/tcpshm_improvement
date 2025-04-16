#pragma once
#include <cstdint>
#include <array>
#include <string>

namespace crypto {

// 1. 五档深度行情数据
struct MarketDepthData 
{
    static constexpr uint16_t msg_type = 1;
    
    uint32_t pair_id;  // 币对ID (0-119)
    int64_t timestamp; // 时间戳 (纳秒)
    
    struct PriceLevel {
        double price;
        double quantity;
    };
    
    PriceLevel bids[5]; // 买单深度
    PriceLevel asks[5]; // 卖单深度
};

// 2. 成交数据
struct TradeData 
{
    static constexpr uint16_t msg_type = 2;
    
    uint32_t pair_id;      // 币对ID (0-119)
    int64_t timestamp;     // 时间戳 (纳秒)
    int64_t trade_id;      // 成交ID
    double price;          // 成交价格
    double quantity;       // 成交数量
    bool is_buyer_maker;   // 是否卖方主动成交
};

// 3. GARCH波动率数据
struct GarchVolatilityData 
{
    static constexpr uint16_t msg_type = 3;
    
    uint32_t pair_id;      // 币对ID (0-119)
    int64_t timestamp;     // 时间戳 (纳秒)
    double volatility;     // GARCH模型计算的波动率
    double forecast_1h;    // 1小时预测波动率
    double forecast_24h;   // 24小时预测波动率
};

// 4. K线数据
struct CandlestickData 
{
    static constexpr uint16_t msg_type = 4;
    
    uint32_t pair_id;      // 币对ID (0-119)
    int64_t timestamp;     // 开始时间戳 (纳秒)
    uint32_t interval;     // 时间间隔(秒)：60=1分钟, 300=5分钟, 3600=1小时
    double open;           // 开盘价
    double high;           // 最高价
    double low;            // 最低价
    double close;          // 收盘价
    double volume;         // 交易量
    double quote_volume;   // 计价币种交易量
};

// 5. 币对状态数据
struct TickerData 
{
    static constexpr uint16_t msg_type = 5;
    
    uint32_t pair_id;         // 币对ID (0-119)
    int64_t timestamp;        // 时间戳 (纳秒)
    double last_price;        // 最新成交价
    double price_change_pct;  // 24小时价格变化百分比
    double high_24h;          // 24小时最高价
    double low_24h;           // 24小时最低价
    double volume_24h;        // 24小时成交量
    uint8_t status;           // 交易状态(0=暂停, 1=交易中)
};

// 批量消息传输的辅助结构
template<typename T, uint16_t BatchMsgType>
struct BatchData 
{
    static constexpr uint16_t msg_type = BatchMsgType;
    
    uint8_t count;  // 包含的数据条目数量
    T entries[1];   // 采用1大小的数组代替0大小的柔性数组
    
    // 估算批量消息的大小
    static uint32_t EstimateSize(uint8_t count) {
        return sizeof(BatchData) - sizeof(T) + count * sizeof(T);
    }
};

// 定义批量数据类型
using BatchMarketDepth = BatchData<MarketDepthData, 101>;
using BatchTrade = BatchData<TradeData, 102>;
using BatchGarchVolatility = BatchData<GarchVolatilityData, 103>;
using BatchCandlestick = BatchData<CandlestickData, 104>;
using BatchTicker = BatchData<TickerData, 105>;

// 货币对名称映射表
struct PairMapping 
{
    uint32_t pair_id;
    char symbol[16]; // 如 "BTC/USDT"
};

} // namespace crypto 