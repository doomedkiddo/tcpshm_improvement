#pragma once

#include <cstdint> // For uint32_t, int64_t, etc.

// configurations that must be the same between server and client
struct CommonConf
{
    static constexpr uint32_t NameSize = 16;
    static constexpr uint32_t ShmQueueSize = 1024 * 1024; // must be power of 2
    static constexpr bool ToLittleEndian = true; // set to the endian of majority of the hosts

    using LoginUserData = char;
    using LoginRspUserData = char;
};

template<uint32_t N, uint16_t MsgType>
struct MsgTpl
{
    static constexpr uint16_t msg_type = MsgType;
    int val[N];
};

typedef MsgTpl<1, 1> Msg1;
typedef MsgTpl<2, 2> Msg2;
typedef MsgTpl<3, 3> Msg3;
typedef MsgTpl<4, 4> Msg4;

// Market depth data with 5 levels
struct MarketDepthMsg
{
    static constexpr uint16_t msg_type = 5;
    
    struct PriceLevel {
        double price;
        int size;
    };
    
    int instrument_id;
    PriceLevel bid[5];
    PriceLevel ask[5];
};

// Trade data
struct TradeMsg
{
    static constexpr uint16_t msg_type = 6;
    
    int instrument_id;
    double price;
    int size;
    int trade_id;
    bool is_buy;
    int64_t timestamp;
};

// Volatility data
struct VolatilityMsg
{
    static constexpr uint16_t msg_type = 7;
    
    int instrument_id;
    double implied_volatility;
    double historical_volatility;
    double realized_volatility;
    int64_t timestamp;
};

// K-line (Candlestick) data
struct KLineMsg
{
    static constexpr uint16_t msg_type = 8;
    
    enum Period {
        MIN_1 = 0,
        MIN_5 = 1,
        MIN_15 = 2,
        MIN_30 = 3,
        HOUR_1 = 4,
        HOUR_4 = 5,
        DAY_1 = 6,
        WEEK_1 = 7,
    };
    
    int instrument_id;
    Period period;
    double open;
    double high;
    double low;
    double close;
    int volume;
    int64_t timestamp;
};

// Ticker data
struct TickerMsg
{
    static constexpr uint16_t msg_type = 9;
    
    int instrument_id;
    double last_price;
    double daily_change;
    double daily_percent_change;
    double daily_high;
    double daily_low;
    int daily_volume;
    int64_t timestamp;
};

