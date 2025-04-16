#include "../tcpshm_server.h"
#include <bits/stdc++.h>
#include "timestamp.h"
#include "common.h"
#include "cpupin.h"
#include <atomic>
#include <random>

using namespace std;
using namespace tcpshm;


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
        cout << "Server stopped" << endl;
    }

private:
    friend TSServer;

    // called with Start()
    // reporting errors on Starting the server
    void OnSystemError(const char* errno_msg, int sys_errno) {
        cout << "System Error: " << errno_msg << " syserrno: " << std::strerror(sys_errno) << endl;
    }

    // called by CTL thread
    // if accept the connection, set user_data in login_rsp and return grpid(start from 0) with respect to tcp or shm
    // else set error_msg in login_rsp if possible, and return -1
    // Note that even if we accept it here, there could be other errors on handling the login,
    // so we have to wait OnClientLogon for confirmation
    int OnNewConnection(const struct sockaddr_in& addr, const LoginMsg* login, LoginRspMsg* login_rsp) {
        cout << "New Connection from: " << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port)
             << ", name: " << login->client_name << ", use_shm: " << static_cast<bool>(login->use_shm) << endl;
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
        cout << "Client file errno, name: " << conn.GetRemoteName() << " reason: " << reason
             << " syserrno: " << std::strerror(sys_errno) << endl;
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
        cout << "Client seq number mismatch, name: " << conn.GetRemoteName() << " ptcp file: " << conn.GetPtcpFile()
             << " local_ack_seq: " << local_ack_seq << " local_seq_start: " << local_seq_start
             << " local_seq_end: " << local_seq_end << " remote_ack_seq: " << remote_ack_seq
             << " remote_seq_start: " << remote_seq_start << " remote_seq_end: " << remote_seq_end << endl;
    }

    // called by CTL thread
    // confirmation for client logon
    void OnClientLogon(const struct sockaddr_in& addr, Connection& conn) {
        cout << "Client Logon from: " << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port)
             << ", name: " << conn.GetRemoteName() << endl;
    }

    // called by CTL thread
    // client is disconnected
    void OnClientDisconnected(Connection& conn, const char* reason, int sys_errno) {
        cout << "Client disconnected,.name: " << conn.GetRemoteName() << " reason: " << reason
             << " syserrno: " << std::strerror(sys_errno) << endl;
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

    static inline std::atomic<bool> stopped{false};
    // set do_cpupin to true to get more stable latency
    bool do_cpupin = true;
    
    // For generating random market data
    std::mt19937 rng;
    std::atomic<int> last_trade_id{0};
};

int main() {
    EchoServer server("server", "server");
    server.Run("0.0.0.0", 12345);

    return 0;
}
