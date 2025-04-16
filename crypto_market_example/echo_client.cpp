#include "../tcpshm_client.h"
#include <bits/stdc++.h>
#include "timestamp.h"
#include "common.h"
#include "cpupin.h"

using namespace std;
using namespace tcpshm;

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
public:
    EchoClient(const std::string& ptcp_dir, const std::string& name)
        : TSClient(ptcp_dir, name)
        , conn(GetConnection()) {
        srand(time(nullptr));
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
            cout << "System Error: " << error_msg << " syserrno: " << strerror(errno) << endl;
            return;
        }
        cout << "client started, send_num: " << *send_num << " recv_num: " << *recv_num << endl;
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
        cout << "client stopped, send_num: " << *send_num << " recv_num: " << *recv_num << " latency: " << latency
             << " avg rtt: " << (msg_sent > 0 ? static_cast<double>(latency) / msg_sent : 0.0) << " ns" << endl;
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

    void handleMarketDepthMsg(MarketDepthMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            cout << "bad: market depth instrument_id: " << v << " expected: " << (*recv_num % 120) << endl;
            exit(1);
        }
        (*recv_num)++;
    }
    
    void handleTradeMsg(TradeMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            cout << "bad: trade instrument_id: " << v << " expected: " << (*recv_num % 120) << endl;
            exit(1);
        }
        (*recv_num)++;
    }
    
    void handleVolatilityMsg(VolatilityMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            cout << "bad: volatility instrument_id: " << v << " expected: " << (*recv_num % 120) << endl;
            exit(1);
        }
        (*recv_num)++;
    }
    
    void handleKLineMsg(KLineMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            cout << "bad: kline instrument_id: " << v << " expected: " << (*recv_num % 120) << endl;
            exit(1);
        }
        (*recv_num)++;
    }
    
    void handleTickerMsg(TickerMsg* msg) {
        int v = msg->instrument_id;
        if(v != (*recv_num % 120)) {
            cout << "bad: ticker instrument_id: " << v << " expected: " << (*recv_num % 120) << endl;
            exit(1);
        }
        (*recv_num)++;
    }

    template<class T>
    void handleMsg(T* msg) {
        for(auto v : msg->val) {
            // convert from configurated network byte order
            Endian<ClientConf::ToLittleEndian>::ConvertInPlace(v);
            if(v != *recv_num) {
                cout << "bad: v: " << v << " recv_num: " << (*recv_num) << endl;
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
        cout << "System Error: " << error_msg << " syserrno: " << strerror(sys_errno) << endl;
    }

    // called within Connect()
    // Login rejected by server
    void OnLoginReject(const LoginRspMsg* login_rsp) {
        cout << "Login Rejected: " << login_rsp->error_msg << endl;
    }

    // called within Connect()
    // confirmation for login success
    int64_t OnLoginSuccess(const LoginRspMsg* login_rsp) {
        cout << "Login Success" << endl;
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
        cout << "Seq number mismatch, name: " << conn.GetRemoteName() << " ptcp file: " << conn.GetPtcpFile()
             << " local_ack_seq: " << local_ack_seq << " local_seq_start: " << local_seq_start
             << " local_seq_end: " << local_seq_end << " remote_ack_seq: " << remote_ack_seq
             << " remote_seq_start: " << remote_seq_start << " remote_seq_end: " << remote_seq_end << endl;
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
        cout << "Client disconnected reason: " << reason << " syserrno: " << strerror(sys_errno) << endl;
    }

private:
    static constexpr int MaxNum = 10000000;
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

