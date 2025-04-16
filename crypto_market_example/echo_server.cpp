#include "../tcpshm_server.h"
#include <bits/stdc++.h>
#include "timestamp.h"
#include "common.h"
#include "cpupin.h"
#include <atomic>
#include <random>
#include <sstream>
#include <iomanip>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

using namespace std;
using namespace tcpshm;

// Helper function for string formatting
template<typename... Args>
std::string format_string(const std::string& format, Args... args) {
    size_t size = snprintf(nullptr, 0, format.c_str(), args...) + 1;
    std::unique_ptr<char[]> buf(new char[size]);
    snprintf(buf.get(), size, format.c_str(), args...);
    return std::string(buf.get(), buf.get() + size - 1);
}

struct ServerConf : public CommonConf
{
  static constexpr int64_t NanoInSecond = 1000000000LL;

  static constexpr uint32_t MaxNewConnections = 5;
  static constexpr uint32_t MaxShmConnsPerGrp = 4;
  static constexpr uint32_t MaxShmGrps = 1;
  static constexpr uint32_t MaxTcpConnsPerGrp = 4;
  static constexpr uint32_t MaxTcpGrps = 1;

  // echo server's TcpQueueSize should be larger than that of client if client is in fast mode
  // otherwise server's send queue could be blocked and ack_seq can only be sent through HB which is slow
  static constexpr uint32_t TcpQueueSize = 4000;       // must be a multiple of 8
  static constexpr uint32_t TcpRecvBufInitSize = 1000; // must be a multiple of 8
  static constexpr uint32_t TcpRecvBufMaxSize = 2000;  // must be a multiple of 8
  static constexpr bool TcpNoDelay = true;

  static constexpr int64_t NewConnectionTimeout = 3 * NanoInSecond;
  static constexpr int64_t ConnectionTimeout = 10 * NanoInSecond;
  static constexpr int64_t HeartBeatInverval = 3 * NanoInSecond;

  using ConnectionUserData = char;
};

class EchoServer;
using TSServer = TcpShmServer<EchoServer, ServerConf>;

class EchoServer : public TSServer
{
public:
    EchoServer(const std::string& ptcp_dir, const std::string& name)
        : TSServer(ptcp_dir, name) {
        // capture SIGTERM to gracefully stop the server
        // we can also send other signals to crash the server and see how it recovers on restart
        std::signal(SIGTERM, EchoServer::SignalHandler);
        
        // Initialize random generator
        rng.seed(std::random_device()());
        
        // Initialize spdlog
        logger = spdlog::stdout_color_mt("server");
        logger->set_level(spdlog::level::info);
    }

    static void SignalHandler(int s) {
        stopped = true;
    }

    void Run(const char* listen_ipv4, uint16_t listen_port) {
        if(!Start(listen_ipv4, listen_port)) return;
        vector<thread> threads;
        // create threads for polling tcp
        for(int i = 0; i < ServerConf::MaxTcpGrps; i++) {
          threads.emplace_back([this, i]() {
            if (do_cpupin) cpupin(4 + i);
            while (!stopped) {
              PollTcp(now(), i);
            }
          });
        }

        // create threads for polling shm
        for(int i = 0; i < ServerConf::MaxShmGrps; i++) {
          threads.emplace_back([this, i]() {
            if (do_cpupin) cpupin(4 + ServerConf::MaxTcpGrps + i);
            while (!stopped) {
              PollShm(i);
            }
          });
        }

        // polling control using this thread
        while(!stopped) {
          PollCtl(now());
        }

        for(auto& thr : threads) {
            thr.join();
        }
        Stop();
        logger->info("Server stopped");
    }

private:
    friend TSServer;

    // called with Start()
    // reporting errors on Starting the server
    void OnSystemError(const char* errno_msg, int sys_errno) {
        logger->error("System Error: {} syserrno: {}", errno_msg, std::strerror(sys_errno));
    }

    // called by CTL thread
    // if accept the connection, set user_data in login_rsp and return grpid(start from 0) with respect to tcp or shm
    // else set error_msg in login_rsp if possible, and return -1
    // Note that even if we accept it here, there could be other errors on handling the login,
    // so we have to wait OnClientLogon for confirmation
    int OnNewConnection(const struct sockaddr_in& addr, const LoginMsg* login, LoginRspMsg* login_rsp) {
        logger->info("New Connection from: {}:{}, name: {}, use_shm: {}", 
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), 
            login->client_name, static_cast<bool>(login->use_shm));
            
        // here we simply hash client name to uniformly map to each group
        auto hh = std::hash<std::string>{}(std::string(login->client_name));
        if(login->use_shm) {
            if(ServerConf::MaxShmGrps > 0) {
                return hh % ServerConf::MaxShmGrps;
            }
            else {
                std::strcpy(login_rsp->error_msg, "Shm disabled");
                return -1;
            }
        }
        else {
            if(ServerConf::MaxTcpGrps > 0) {
                return hh % ServerConf::MaxTcpGrps;
            }
            else {
                std::strcpy(login_rsp->error_msg, "Tcp disabled");
                return -1;
            }
        }
    }

    // called by CTL thread
    // ptcp or shm files can't be open or are corrupt
    void OnClientFileError(Connection& conn, const char* reason, int sys_errno) {
        logger->error("Client file errno, name: {} reason: {} syserrno: {}", 
            conn.GetRemoteName(), reason, std::strerror(sys_errno));
    }

    // called by CTL thread
    // server and client ptcp sequence number don't match, we need to fix it manually
    void OnSeqNumberMismatch(Connection& conn,
                             uint32_t local_ack_seq,
                             uint32_t local_seq_start,
                             uint32_t local_seq_end,
                             uint32_t remote_ack_seq,
                             uint32_t remote_seq_start,
                             uint32_t remote_seq_end) {
        logger->warn("Client seq number mismatch, name: {} ptcp file: {} local_ack_seq: {} local_seq_start: {} "
             "local_seq_end: {} remote_ack_seq: {} remote_seq_start: {} remote_seq_end: {}", 
             conn.GetRemoteName(), conn.GetPtcpFile(), local_ack_seq, local_seq_start, local_seq_end, 
             remote_ack_seq, remote_seq_start, remote_seq_end);
    }

    // called by CTL thread
    // confirmation for client logon
    void OnClientLogon(const struct sockaddr_in& addr, Connection& conn) {
        logger->info("Client Logon from: {}:{}, name: {}", 
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), conn.GetRemoteName());
    }

    // called by CTL thread
    // client is disconnected
    void OnClientDisconnected(Connection& conn, const char* reason, int sys_errno) {
        logger->info("Client disconnected, name: {} reason: {} syserrno: {}", 
            conn.GetRemoteName(), reason, std::strerror(sys_errno));
    }

    // called by APP thread
    void OnClientMsg(Connection& conn, MsgHeader* recv_header) {
        int msg_type = recv_header->msg_type;
        
        // Parse the instrument ID from the incoming message
        int instrument_id = 0;
        if (msg_type >= 5 && msg_type <= 9) {
            // For all market data messages
            void* msg_body = recv_header + 1;
            instrument_id = *reinterpret_cast<int*>(msg_body);
            
            // 在接收到请求时打印请求信息
            std::string symbol = GetSymbolName(instrument_id);
            logger->info("收到请求 - 类型: {}, 币对: {} (ID: {})", msg_type, symbol, instrument_id);
        }
        
        // Respond with the appropriate market data message
        switch (msg_type) {
            case 1:
            case 2:
            case 3:
            case 4:
            {
                // Handle legacy message types (1-4)
                auto size = recv_header->size - sizeof(MsgHeader);
                MsgHeader* send_header = conn.Alloc(size);
                if(!send_header) return;
                send_header->msg_type = recv_header->msg_type;
                std::memcpy(send_header + 1, recv_header + 1, size);
                conn.Pop();
                conn.Push();
                break;
            }
                
            case 5: // Market Depth
                SendMarketDepthMsg(conn, instrument_id);
                conn.Pop();
                break;
                
            case 6: // Trade
                SendTradeMsg(conn, instrument_id);
                conn.Pop();
                break;
                
            case 7: // Volatility
                SendVolatilityMsg(conn, instrument_id);
                conn.Pop();
                break;
                
            case 8: // K-Line
                SendKLineMsg(conn, instrument_id);
                conn.Pop();
                break;
                
            case 9: // Ticker
                SendTickerMsg(conn, instrument_id);
                conn.Pop();
                break;
                
            default:
            {
                // Unknown message type, just echo it back
                auto size = recv_header->size - sizeof(MsgHeader);
                MsgHeader* send_header = conn.Alloc(size);
                if(!send_header) return;
                send_header->msg_type = recv_header->msg_type;
                std::memcpy(send_header + 1, recv_header + 1, size);
                conn.Pop();
                conn.Push();
                break;
            }
        }
    }
    
    void SendMarketDepthMsg(Connection& conn, int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(MarketDepthMsg));
        if(!header) return;
        
        header->msg_type = MarketDepthMsg::msg_type;
        MarketDepthMsg* msg = reinterpret_cast<MarketDepthMsg*>(header + 1);
        
        // Make sure instrument_id is in the valid range (0-119)
        msg->instrument_id = instrument_id % 120;
        
        // Base price for this instrument
        double base_price = 100.0 + (msg->instrument_id % 100);
        
        // Generate some test prices and sizes for 5 levels
        for(int i = 0; i < 5; i++) {
            msg->bid[i].price = base_price - i * 0.1 - RandomOffset();
            msg->bid[i].size = 100 + i * 10 + RandomSize();
            msg->ask[i].price = base_price + 0.1 + i * 0.1 + RandomOffset();
            msg->ask[i].size = 100 + i * 10 + RandomSize();
        }
        
        // Generate symbol name (e.g., BTC-USDT, ETH-USDT, etc.)
        std::string symbol = GetSymbolName(msg->instrument_id);
        
        // Print the data being sent
        logger->info("发送市场深度数据 - 币对: {} (ID: {}) 最优买价: {:.2f} 最优卖价: {:.2f}", 
            symbol, msg->instrument_id, msg->bid[0].price, msg->ask[0].price);
        
        conn.Push();
    }
    
    void SendTradeMsg(Connection& conn, int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(TradeMsg));
        if(!header) return;
        
        header->msg_type = TradeMsg::msg_type;
        TradeMsg* msg = reinterpret_cast<TradeMsg*>(header + 1);
        
        // Make sure instrument_id is in the valid range (0-119)
        msg->instrument_id = instrument_id % 120;
        
        // Base price for this instrument
        double base_price = 100.0 + (msg->instrument_id % 100);
        
        // Generate trade data
        msg->price = base_price + RandomOffset();
        msg->size = 100 + RandomSize();
        msg->trade_id = ++last_trade_id;
        msg->is_buy = (RandomSize() % 2 == 0);
        msg->timestamp = now();
        
        // Generate symbol name
        std::string symbol = GetSymbolName(msg->instrument_id);
        
        // Print the data being sent
        logger->info("发送成交数据 - 币对: {} (ID: {}) 价格: {:.2f} 数量: {} 方向: {}", 
            symbol, msg->instrument_id, msg->price, msg->size, msg->is_buy ? "买入" : "卖出");
        
        conn.Push();
    }
    
    void SendVolatilityMsg(Connection& conn, int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(VolatilityMsg));
        if(!header) return;
        
        header->msg_type = VolatilityMsg::msg_type;
        VolatilityMsg* msg = reinterpret_cast<VolatilityMsg*>(header + 1);
        
        // Make sure instrument_id is in the valid range (0-119)
        msg->instrument_id = instrument_id % 120;
        
        // Generate volatility data
        msg->implied_volatility = 0.2 + (msg->instrument_id % 10) * 0.01 + RandomOffset() * 0.1;
        msg->historical_volatility = 0.18 + (msg->instrument_id % 8) * 0.01 + RandomOffset() * 0.1;
        msg->realized_volatility = 0.19 + (msg->instrument_id % 9) * 0.01 + RandomOffset() * 0.1;
        msg->timestamp = now();
        
        // Generate symbol name
        std::string symbol = GetSymbolName(msg->instrument_id);
        
        // Print the data being sent
        logger->info("发送波动率数据 - 币对: {} (ID: {}) 隐含波动率: {:.4f} 历史波动率: {:.4f} 实际波动率: {:.4f}", 
            symbol, msg->instrument_id, msg->implied_volatility, msg->historical_volatility, msg->realized_volatility);
        
        conn.Push();
    }
    
    void SendKLineMsg(Connection& conn, int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(KLineMsg));
        if(!header) return;
        
        header->msg_type = KLineMsg::msg_type;
        KLineMsg* msg = reinterpret_cast<KLineMsg*>(header + 1);
        
        // Make sure instrument_id is in the valid range (0-119)
        msg->instrument_id = instrument_id % 120;
        
        // Base price for this instrument
        double base_price = 100.0 + (msg->instrument_id % 100);
        
        // Random period
        msg->period = static_cast<KLineMsg::Period>(RandomSize() % 8);
        
        // Generate candle data
        double range = 1.0 + (RandomOffset() * 0.5);
        msg->close = base_price + RandomOffset();
        msg->open = msg->close - RandomOffset();
        msg->high = std::max(msg->open, msg->close) + (range * 0.2);
        msg->low = std::min(msg->open, msg->close) - (range * 0.2);
        msg->volume = 1000 + RandomSize() * 10;
        msg->timestamp = now();
        
        // Map period enum to string for display
        std::string period_str;
        switch(msg->period) {
            case KLineMsg::MIN_1: period_str = "1分钟"; break;
            case KLineMsg::MIN_5: period_str = "5分钟"; break;
            case KLineMsg::MIN_15: period_str = "15分钟"; break;
            case KLineMsg::MIN_30: period_str = "30分钟"; break;
            case KLineMsg::HOUR_1: period_str = "1小时"; break;
            case KLineMsg::HOUR_4: period_str = "4小时"; break;
            case KLineMsg::DAY_1: period_str = "1天"; break;
            case KLineMsg::WEEK_1: period_str = "1周"; break;
        }
        
        // Generate symbol name
        std::string symbol = GetSymbolName(msg->instrument_id);
        
        // Print the data being sent
        logger->info("发送K线数据 - 币对: {} (ID: {}) 周期: {} 开: {:.2f} 高: {:.2f} 低: {:.2f} 收: {:.2f} 量: {}", 
            symbol, msg->instrument_id, period_str, msg->open, msg->high, msg->low, msg->close, msg->volume);
        
        conn.Push();
    }
    
    void SendTickerMsg(Connection& conn, int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(TickerMsg));
        if(!header) return;
        
        header->msg_type = TickerMsg::msg_type;
        TickerMsg* msg = reinterpret_cast<TickerMsg*>(header + 1);
        
        // Make sure instrument_id is in the valid range (0-119)
        msg->instrument_id = instrument_id % 120;
        
        // Base price for this instrument
        double base_price = 100.0 + (msg->instrument_id % 100);
        
        // Generate ticker data
        msg->last_price = base_price + RandomOffset();
        msg->daily_change = RandomOffset() * 2.0;
        msg->daily_percent_change = (msg->daily_change / base_price) * 100.0;
        msg->daily_high = base_price + std::abs(RandomOffset() * 2.0);
        msg->daily_low = base_price - std::abs(RandomOffset() * 2.0);
        msg->daily_volume = 10000 + RandomSize() * 100;
        msg->timestamp = now();
        
        // Generate symbol name
        std::string symbol = GetSymbolName(msg->instrument_id);
        
        // Print the data being sent
        logger->info("发送行情数据 - 币对: {} (ID: {}) 最新价: {:.2f} 涨跌幅: {:.2f}% 高: {:.2f} 低: {:.2f} 量: {}", 
            symbol, msg->instrument_id, msg->last_price, msg->daily_percent_change, 
            msg->daily_high, msg->daily_low, msg->daily_volume);
        
        conn.Push();
    }
    
    // Helper function to generate small random price offsets
    double RandomOffset() {
        std::uniform_real_distribution<> dist(-0.5, 0.5);
        return dist(rng);
    }
    
    // Helper function to generate random sizes
    int RandomSize() {
        std::uniform_int_distribution<> dist(1, 100);
        return dist(rng);
    }

    // Helper function to generate a symbol name based on instrument_id
    std::string GetSymbolName(int instrument_id) {
        // Common cryptocurrency symbols
        static const std::vector<std::string> base_currencies = {
            "BTC", "ETH", "BNB", "XRP", "ADA", "SOL", "DOT", "DOGE", "AVAX", "MATIC",
            "LINK", "UNI", "ATOM", "LTC", "FTM", "ALGO", "XLM", "VET", "AXS", "FIL"
        };
        
        // Common quote currencies
        static const std::vector<std::string> quote_currencies = {
            "USDT", "USDC", "BUSD", "DAI", "USD", "EUR"
        };
        
        int base_idx = instrument_id % base_currencies.size();
        int quote_idx = (instrument_id / base_currencies.size()) % quote_currencies.size();
        
        return base_currencies[base_idx] + "-" + quote_currencies[quote_idx];
    }

    static inline std::atomic<bool> stopped{false};
    // set do_cpupin to true to get more stable latency
    bool do_cpupin = true;
    
    // For generating random market data
    std::mt19937 rng;
    std::atomic<int> last_trade_id{0};
    std::shared_ptr<spdlog::logger> logger;
};

int main() {
    EchoServer server("server", "server");
    server.Run("0.0.0.0", 12345);

    return 0;
}
