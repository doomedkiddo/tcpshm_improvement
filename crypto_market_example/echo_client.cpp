#include "../tcpshm_client.h"
#include <bits/stdc++.h>
#include "timestamp.h"
#include "common.h"
#include "cpupin.h"
#include <sstream>
#include <iomanip>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <chrono>
#include <thread>
#include <map>
#include <numeric>

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

struct ClientConf : public CommonConf
{
  static constexpr int64_t NanoInSecond = 1000000000LL;

  static constexpr uint32_t TcpQueueSize = 2000;       // must be a multiple of 8
  static constexpr uint32_t TcpRecvBufInitSize = 1000; // must be a multiple of 8
  static constexpr uint32_t TcpRecvBufMaxSize = 2000;  // must be a multiple of 8
  static constexpr bool TcpNoDelay = true;

  static constexpr int64_t ConnectionTimeout = 10 * NanoInSecond;
  static constexpr int64_t HeartBeatInverval = 3 * NanoInSecond;

  using ConnectionUserData = char;
};

class EchoClient;
using TSClient = TcpShmClient<EchoClient, ClientConf>;

class EchoClient : public TSClient
{
private:
    // 延迟统计结构
    struct LatencyStats {
        uint64_t min = UINT64_MAX;
        uint64_t max = 0;
        uint64_t total = 0;
        int count = 0;
        
        void update(uint64_t latency) {
            min = std::min(min, latency);
            max = std::max(max, latency);
            total += latency;
            count++;
        }
    };

public:
    EchoClient(const std::string& ptcp_dir, const std::string& name)
        : TSClient(ptcp_dir, name)
        , conn(GetConnection()) {
        srand(time(nullptr));
        
        // Initialize spdlog
        logger = spdlog::stdout_color_mt("client");
        logger->set_level(spdlog::level::info);
    }

    void Run(bool use_shm, const char* server_ipv4, uint16_t server_port) {
        if(!Connect(use_shm, server_ipv4, server_port, 0)) return;
        // we mmap the send and recv number to file in case of program crash
        string send_num_file =
            string(conn.GetPtcpDir()) + "/" + conn.GetLocalName() + "_" + conn.GetRemoteName() + ".send_num";
        string recv_num_file =
            string(conn.GetPtcpDir()) + "/" + conn.GetLocalName() + "_" + conn.GetRemoteName() + ".recv_num";
        const char* error_msg;
        send_num = my_mmap<int>(send_num_file.c_str(), false, &error_msg);
        recv_num = my_mmap<int>(recv_num_file.c_str(), false, &error_msg);
        if(!send_num || !recv_num) {
            logger->error("系统错误: {} 系统错误码: {}", error_msg, strerror(errno));
            return;
        }
        
        // 重置计数器，从0开始
        logger->info("重置计数器，原始值: send_num: {} recv_num: {}", *send_num, *recv_num);
        *send_num = 0;
        *recv_num = 0;
        
        // 初始化延迟统计
        latency_stats.clear();
        for (int i = 5; i <= 9; i++) {
            latency_stats[i] = LatencyStats{};
        }
        msg_sent_time.clear();
        
        logger->info("客户端已启动, send_num: {} recv_num: {}", *send_num, *recv_num);
        if(use_shm) {
            thread shm_thr([this]() {
                if(do_cpupin) cpupin(7);
                start_time = now();
                while(!conn.IsClosed()) {
                    if(PollNum()) {
                        stop_time = now();
                        conn.Close();
                        break;
                    }
                    PollShm();
                }
            });

            // we still need to poll tcp for heartbeats even if using shm
            while(!conn.IsClosed()) {
              PollTcp(now());
            }
            shm_thr.join();
        }
        else {
            if(do_cpupin) cpupin(7);
            start_time = now();
            while(!conn.IsClosed()) {
                if(PollNum()) {
                    stop_time = now();
                    conn.Close();
                    break;
                }
                PollTcp(now());
            }
        }
        uint64_t latency = stop_time - start_time;
        Stop();
        
        // 输出详细的延迟统计信息
        PrintLatencyStats(latency);
    }
    
    // 输出详细的延迟统计信息
    void PrintLatencyStats(uint64_t total_latency) {
        logger->info("客户端已停止, 发送消息: {} 接收消息: {} 总延迟: {} 纳秒", 
            *send_num, *recv_num, total_latency);
        
        // 总体延迟信息
        double avg_rtt = (msg_sent > 0 ? static_cast<double>(total_latency) / msg_sent : 0.0);
        logger->info("平均往返延迟: {:.2f} 纳秒 ({:.3f} 毫秒)", avg_rtt, avg_rtt / 1000000.0);
        
        // 按消息类型统计
        static const std::map<int, std::string> msg_type_names = {
            {5, "市场深度数据"}, 
            {6, "成交数据"}, 
            {7, "波动率数据"}, 
            {8, "K线数据"}, 
            {9, "行情数据"}
        };
        
        logger->info("--- 按消息类型统计延迟 (纳秒) ---");
        for (const auto& [type, stats] : latency_stats) {
            if (stats.count == 0) continue;
            
            double avg = stats.total / static_cast<double>(stats.count);
            logger->info("{}: 数量: {}, 最小: {}, 最大: {}, 平均: {:.2f} 纳秒 ({:.3f} 毫秒)", 
                msg_type_names.at(type), stats.count, stats.min, stats.max, avg, avg / 1000000.0);
        }
    }

private:
    bool PollNum() {
        if(*send_num < MaxNum) {
            // for slow mode, we wait to recv an echo msg before sending the next one
            if(slow && *send_num != *recv_num) return false;
            
            // Send a market data request based on the current message count
            int msg_type = 5 + (*send_num % 5); // Cycle through message types 5-9
            int instrument_id = *send_num % 120; // Cycle through 120 instruments
            
            switch(msg_type) {
                case 5: TrySendMarketDepthRequest(instrument_id); break;
                case 6: TrySendTradeRequest(instrument_id); break;
                case 7: TrySendVolatilityRequest(instrument_id); break;
                case 8: TrySendKLineRequest(instrument_id); break;
                case 9: TrySendTickerRequest(instrument_id); break;
                default: break;
            }
            
            // 添加短暂延迟，以便能看到输出
            // std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        else {
            // if all echo msgs are got, we are done
            if(*send_num == *recv_num) return true;
        }
        return false;
    }
    
    bool TrySendMarketDepthRequest(int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(int));
        if(!header) return false;
        header->msg_type = MarketDepthMsg::msg_type;
        int* id = reinterpret_cast<int*>(header + 1);
        *id = instrument_id;
        (*send_num)++;
        
        // Get symbol name
        std::string symbol = GetSymbolName(instrument_id);
        
        // 记录发送时间
        uint64_t send_time = now();
        msg_sent_time[*send_num] = {header->msg_type, send_time};
        
        logger->info("发送市场深度请求 - 币对: {} (ID: {}), 请求次数: {}/{}", 
            symbol, instrument_id, *send_num, MaxNum);
        conn.Push();
        msg_sent++;
        return true;
    }
    
    bool TrySendTradeRequest(int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(int));
        if(!header) return false;
        header->msg_type = TradeMsg::msg_type;
        int* id = reinterpret_cast<int*>(header + 1);
        *id = instrument_id;
        (*send_num)++;
        
        // Get symbol name
        std::string symbol = GetSymbolName(instrument_id);
        
        // 记录发送时间
        uint64_t send_time = now();
        msg_sent_time[*send_num] = {header->msg_type, send_time};
        
        logger->info("发送成交数据请求 - 币对: {} (ID: {}), 请求次数: {}/{}", 
            symbol, instrument_id, *send_num, MaxNum);
        conn.Push();
        msg_sent++;
        return true;
    }
    
    bool TrySendVolatilityRequest(int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(int));
        if(!header) return false;
        header->msg_type = VolatilityMsg::msg_type;
        int* id = reinterpret_cast<int*>(header + 1);
        *id = instrument_id;
        (*send_num)++;
        
        // Get symbol name
        std::string symbol = GetSymbolName(instrument_id);
        
        // 记录发送时间
        uint64_t send_time = now();
        msg_sent_time[*send_num] = {header->msg_type, send_time};
        
        logger->info("发送波动率数据请求 - 币对: {} (ID: {}), 请求次数: {}/{}", 
            symbol, instrument_id, *send_num, MaxNum);
        conn.Push();
        msg_sent++;
        return true;
    }
    
    bool TrySendKLineRequest(int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(int));
        if(!header) return false;
        header->msg_type = KLineMsg::msg_type;
        int* id = reinterpret_cast<int*>(header + 1);
        *id = instrument_id;
        (*send_num)++;
        
        // Get symbol name
        std::string symbol = GetSymbolName(instrument_id);
        
        // 记录发送时间
        uint64_t send_time = now();
        msg_sent_time[*send_num] = {header->msg_type, send_time};
        
        logger->info("发送K线数据请求 - 币对: {} (ID: {}), 请求次数: {}/{}", 
            symbol, instrument_id, *send_num, MaxNum);
        conn.Push();
        msg_sent++;
        return true;
    }
    
    bool TrySendTickerRequest(int instrument_id) {
        MsgHeader* header = conn.Alloc(sizeof(int));
        if(!header) return false;
        header->msg_type = TickerMsg::msg_type;
        int* id = reinterpret_cast<int*>(header + 1);
        *id = instrument_id;
        (*send_num)++;
        
        // Get symbol name
        std::string symbol = GetSymbolName(instrument_id);
        
        // 记录发送时间
        uint64_t send_time = now();
        msg_sent_time[*send_num] = {header->msg_type, send_time};
        
        logger->info("发送行情数据请求 - 币对: {} (ID: {}), 请求次数: {}/{}", 
            symbol, instrument_id, *send_num, MaxNum);
        conn.Push();
        msg_sent++;
        return true;
    }

    template<class T>
    bool TrySendMsg() {
        MsgHeader* header = conn.Alloc(sizeof(T));
        if(!header) return false;
        header->msg_type = T::msg_type;
        T* msg = reinterpret_cast<T*>(header + 1);
        for(auto& v : msg->val) {
            // convert to configurated network byte order, don't need this if you know server is using the same endian
            v = Endian<ClientConf::ToLittleEndian>::Convert((*send_num)++);
        }
        conn.Push();
        msg_sent++;
        return true;
    }

    // 添加币对名称生成函数
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

    void handleMarketDepthMsg(MarketDepthMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            logger->error("错误: 市场深度数据 ID: {} 期望值: {}", v, (*recv_num % 120));
            exit(1);
        }
        
        // 计算延迟
        uint64_t recv_time = now();
        auto it = msg_sent_time.find(*recv_num);
        if (it != msg_sent_time.end()) {
            uint64_t latency = recv_time - it->second.second;
            latency_stats[it->second.first].update(latency);
            
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data with latency
            logger->info("接收市场深度数据 - 币对: {} (ID: {}) 最优买价: {:.2f} 最优卖价: {:.2f}, 延迟: {} 纳秒, 接收次数: {}/{}", 
                symbol, msg->instrument_id, msg->bid[0].price, msg->ask[0].price, latency, *recv_num + 1, MaxNum);
            
            msg_sent_time.erase(it);
        } else {
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data without latency
            logger->info("接收市场深度数据 - 币对: {} (ID: {}) 最优买价: {:.2f} 最优卖价: {:.2f}, 接收次数: {}/{}", 
                symbol, msg->instrument_id, msg->bid[0].price, msg->ask[0].price, *recv_num + 1, MaxNum);
        }
        
        (*recv_num)++;
    }
    
    void handleTradeMsg(TradeMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            logger->error("错误: 成交数据 ID: {} 期望值: {}", v, (*recv_num % 120));
            exit(1);
        }
        
        // 计算延迟
        uint64_t recv_time = now();
        auto it = msg_sent_time.find(*recv_num);
        if (it != msg_sent_time.end()) {
            uint64_t latency = recv_time - it->second.second;
            latency_stats[it->second.first].update(latency);
            
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data with latency
            logger->info("接收成交数据 - 币对: {} (ID: {}) 价格: {:.2f} 数量: {} 方向: {}, 延迟: {} 纳秒, 接收次数: {}/{}", 
                symbol, msg->instrument_id, msg->price, msg->size, msg->is_buy ? "买入" : "卖出", 
                latency, *recv_num + 1, MaxNum);
            
            msg_sent_time.erase(it);
        } else {
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data without latency
            logger->info("接收成交数据 - 币对: {} (ID: {}) 价格: {:.2f} 数量: {} 方向: {}, 接收次数: {}/{}", 
                symbol, msg->instrument_id, msg->price, msg->size, msg->is_buy ? "买入" : "卖出", 
                *recv_num + 1, MaxNum);
        }
        
        (*recv_num)++;
    }
    
    void handleVolatilityMsg(VolatilityMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            logger->error("错误: 波动率数据 ID: {} 期望值: {}", v, (*recv_num % 120));
            exit(1);
        }
        
        // 计算延迟
        uint64_t recv_time = now();
        auto it = msg_sent_time.find(*recv_num);
        if (it != msg_sent_time.end()) {
            uint64_t latency = recv_time - it->second.second;
            latency_stats[it->second.first].update(latency);
            
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data with latency
            logger->info("接收波动率数据 - 币对: {} (ID: {}) 隐含波动率: {:.4f} 历史波动率: {:.4f} 实际波动率: {:.4f}, 延迟: {} 纳秒, 接收次数: {}/{}", 
                symbol, msg->instrument_id, msg->implied_volatility, msg->historical_volatility, msg->realized_volatility, 
                latency, *recv_num + 1, MaxNum);
            
            msg_sent_time.erase(it);
        } else {
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data without latency
            logger->info("接收波动率数据 - 币对: {} (ID: {}) 隐含波动率: {:.4f} 历史波动率: {:.4f} 实际波动率: {:.4f}, 接收次数: {}/{}", 
                symbol, msg->instrument_id, msg->implied_volatility, msg->historical_volatility, msg->realized_volatility, 
                *recv_num + 1, MaxNum);
        }
        
        (*recv_num)++;
    }
    
    void handleKLineMsg(KLineMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            logger->error("错误: K线数据 ID: {} 期望值: {}", v, (*recv_num % 120));
            exit(1);
        }
        
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
        
        // 计算延迟
        uint64_t recv_time = now();
        auto it = msg_sent_time.find(*recv_num);
        if (it != msg_sent_time.end()) {
            uint64_t latency = recv_time - it->second.second;
            latency_stats[it->second.first].update(latency);
            
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data with latency
            logger->info("接收K线数据 - 币对: {} (ID: {}) 周期: {} 开: {:.2f} 高: {:.2f} 低: {:.2f} 收: {:.2f} 量: {}, 延迟: {} 纳秒, 接收次数: {}/{}", 
                symbol, msg->instrument_id, period_str, msg->open, msg->high, msg->low, msg->close, msg->volume, 
                latency, *recv_num + 1, MaxNum);
            
            msg_sent_time.erase(it);
        } else {
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data without latency
            logger->info("接收K线数据 - 币对: {} (ID: {}) 周期: {} 开: {:.2f} 高: {:.2f} 低: {:.2f} 收: {:.2f} 量: {}, 接收次数: {}/{}", 
                symbol, msg->instrument_id, period_str, msg->open, msg->high, msg->low, msg->close, msg->volume, 
                *recv_num + 1, MaxNum);
        }
        
        (*recv_num)++;
    }
    
    void handleTickerMsg(TickerMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            logger->error("错误: 行情数据 ID: {} 期望值: {}", v, (*recv_num % 120));
            exit(1);
        }
        
        // 计算延迟
        uint64_t recv_time = now();
        auto it = msg_sent_time.find(*recv_num);
        if (it != msg_sent_time.end()) {
            uint64_t latency = recv_time - it->second.second;
            latency_stats[it->second.first].update(latency);
            
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data with latency
            logger->info("接收行情数据 - 币对: {} (ID: {}) 最新价: {:.2f} 涨跌幅: {:.2f}% 高: {:.2f} 低: {:.2f} 量: {}, 延迟: {} 纳秒, 接收次数: {}/{}", 
                symbol, msg->instrument_id, msg->last_price, msg->daily_percent_change, 
                msg->daily_high, msg->daily_low, msg->daily_volume, latency, *recv_num + 1, MaxNum);
            
            msg_sent_time.erase(it);
        } else {
            // Get symbol name
            std::string symbol = GetSymbolName(msg->instrument_id);
            
            // Print the received data without latency
            logger->info("接收行情数据 - 币对: {} (ID: {}) 最新价: {:.2f} 涨跌幅: {:.2f}% 高: {:.2f} 低: {:.2f} 量: {}, 接收次数: {}/{}", 
                symbol, msg->instrument_id, msg->last_price, msg->daily_percent_change, 
                msg->daily_high, msg->daily_low, msg->daily_volume, *recv_num + 1, MaxNum);
        }
        
        (*recv_num)++;
    }

    template<class T>
    void handleMsg(T* msg) {
        for(auto v : msg->val) {
            // convert from configurated network byte order
            Endian<ClientConf::ToLittleEndian>::ConvertInPlace(v);
            if(v != *recv_num) {
                logger->error("错误: 值 {} 不符合预期: {}", v, (*recv_num));
                exit(1);
            }
            (*recv_num)++;
        }
    }

private:
    friend TSClient;
    // called within Connect()
    // reporting errors on connecting to the server
    void OnSystemError(const char* error_msg, int sys_errno) {
        logger->error("系统错误: {} 系统错误码: {}", error_msg, strerror(sys_errno));
    }

    // called within Connect()
    // Login rejected by server
    void OnLoginReject(const LoginRspMsg* login_rsp) {
        logger->error("登录被拒绝: {}", login_rsp->error_msg);
    }

    // called within Connect()
    // confirmation for login success
    int64_t OnLoginSuccess(const LoginRspMsg* login_rsp) {
        logger->info("登录成功");
        return now();
    }

    // called within Connect()
    // server and client ptcp sequence number don't match, we need to fix it manually
    void OnSeqNumberMismatch(uint32_t local_ack_seq,
                             uint32_t local_seq_start,
                             uint32_t local_seq_end,
                             uint32_t remote_ack_seq,
                             uint32_t remote_seq_start,
                             uint32_t remote_seq_end) {
        logger->warn("序列号不匹配, 名称: {} ptcp文件: {} local_ack_seq: {} local_seq_start: {} "
             "local_seq_end: {} remote_ack_seq: {} remote_seq_start: {} remote_seq_end: {}", 
             conn.GetRemoteName(), conn.GetPtcpFile(), local_ack_seq, local_seq_start, local_seq_end, 
             remote_ack_seq, remote_seq_start, remote_seq_end);
    }

    // called by APP thread
    void OnServerMsg(MsgHeader* header) {
        // auto msg_type = header->msg_type;
        switch(header->msg_type) {
            case 1: handleMsg(reinterpret_cast<Msg1*>(header + 1)); break;
            case 2: handleMsg(reinterpret_cast<Msg2*>(header + 1)); break;
            case 3: handleMsg(reinterpret_cast<Msg3*>(header + 1)); break;
            case 4: handleMsg(reinterpret_cast<Msg4*>(header + 1)); break;
            case 5: handleMarketDepthMsg(reinterpret_cast<MarketDepthMsg*>(header + 1)); break;
            case 6: handleTradeMsg(reinterpret_cast<TradeMsg*>(header + 1)); break;
            case 7: handleVolatilityMsg(reinterpret_cast<VolatilityMsg*>(header + 1)); break;
            case 8: handleKLineMsg(reinterpret_cast<KLineMsg*>(header + 1)); break;
            case 9: handleTickerMsg(reinterpret_cast<TickerMsg*>(header + 1)); break;
            default: assert(false);
        }
        conn.Pop();
    }

    // called by tcp thread
    void OnDisconnected(const char* reason, int sys_errno) {
        logger->info("客户端断开连接 原因: {} 系统错误码: {}", reason, strerror(sys_errno));
    }

private:
    static constexpr int MaxNum = 100;
    Connection& conn;
    int msg_sent = 0;
    uint64_t start_time = 0;
    uint64_t stop_time = 0;
    // set slow to false to send msgs as fast as it can
    bool slow = false;
    // set do_cpupin to true to get more stable latency
    bool do_cpupin = true;
    int* send_num;
    int* recv_num;
    std::shared_ptr<spdlog::logger> logger;
    
    // 延迟统计相关数据
    std::map<int, LatencyStats> latency_stats; // 每种消息类型的延迟统计
    std::map<uint64_t, std::pair<int, uint64_t>> msg_sent_time; // 消息ID -> (消息类型, 发送时间)
};

int main(int argc, const char** argv) {
    if(argc != 4) {
        cout << "usage: echo_client NAME SERVER_IP USE_SHM[0|1]" << endl;
        exit(1);
    }
    const char* name = argv[1];
    const char* server_ip = argv[2];
    bool use_shm = argv[3][0] != '0';

    EchoClient client(name, name);
    client.Run(use_shm, server_ip, 12345);

    return 0;
}

