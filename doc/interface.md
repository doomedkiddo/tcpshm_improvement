tcpshm
======

## 消息头和TCP共享内存连接
每个消息都会自动添加一个`MsgHeader`（消息头），无论是控制消息还是应用消息，它都是一个8字节的结构体，使用主机字节序：

```c++
struct MsgHeader
{
    // size of this msg, including header itself
    // auto set by lib, can be read by user
    uint16_t size;
    // msg type of app msg is set by user and must not be 0
    uint16_t msg_type;
    // internally used for ptcp, must not be modified by user
    uint32_t ack_seq;
};
```
如果通过TCP通道发送，框架会自动对MsgHeader进行字节序转换（参见下面的ToLittleEndian配置）。

TcpShmConnection是一个通用的连接类，我们可以用它来发送或接收消息。
**注意：在同一个连接上读写消息必须在同一个线程中进行：即它的轮询线程（参见[限制](https://github.com/MengRao/tcpshm#limitations)）。**
对于发送，用户调用Alloc()来分配空间保存消息：

```c++
    // allocate a msg of specified size in send queue
    // the returned address is guaranteed to be 8 byte aligned
    // return nullptr if no enough space
    MsgHeader* Alloc(uint16_t size);
```

在返回的`MsgHeader`指针中，用户需要设置msg_type字段以及头部后面的消息内容（消息内容的字节序处理是用户自己的责任），然后调用Push()提交并发送消息。
如果用户需要连续发送多个消息，最好为前几个消息使用PushMore()，为最后一个消息使用Push()：
```c++
    // submit the last msg from Alloc() and send out
    void Push();

    // for shm, same as Push
    // for tcp, don't send out immediately as we have more to push
    void PushMore();
```

对于接收，用户调用Front()获取接收队列中的第一个应用消息，但通常情况下，Front()应该由框架在轮询函数中自动调用：
```c++
    // get the next msg from recv queue, return nullptr if queue is empty
    // the returned address is guaranteed to be 8 byte aligned
    // if caller dont call Pop() later, it will get the same msg again
    // user dont need to call Front() directly as polling functions will do it
    MsgHeader* Front();
```
如果返回的`MsgHeader`不是nullptr，用户可以从它的msg_type和size识别基本的消息信息，并处理`MsgHeader`后面的消息内容。
如果用户完成了消息处理，应该调用Pop()来消费它，否则用户将在下一次Front()中再次获得相同的消息：
```c++
    // consume the msg we got from Front() or polling function
    void Pop();
```

在一个典型的场景中，当处理一个消息时，用户想要立即发送回一个响应消息，应该按顺序调用Pop()和Push()而不是相反的顺序，原因是：
1) 对于TCP，Push()会发送到网络，这会比较慢，所以如果我们按相反顺序操作，当程序崩溃时，已提交的消息可能保存在发送队列中而Pop()未被调用，那么在恢复时它将再次处理相同的消息并推送一个重复的响应。如果我们按Pop()和Push()的顺序，仍然有一种可能性是Pop()成功但Push()失败（错过发送响应），但这只是一个理论上的可能性，您可以测试EchoServer示例。  
2) 对于TCP，如果我们调用Pop()和Push()，更新的确认序列号（由于Pop()）将会被响应消息（由于Push()）捎带，这意味着远程端将更快地获得更新。

用户可以关闭连接，远程端将收到断开连接的通知。
```c++
    // Close this connection
    void Close();
```

在应用程序中，用户不允许创建TcpShmConnection，但可以从客户端或服务器框架获取对它的引用，这个引用保证在服务器/客户端停止之前一直有效，这允许用户即使在断开连接的情况下仍然可以发送消息，远程端将在重新建立连接后收到它们。

连接相关的配置如下：
```c++
struct Conf
{
    // the size of client/server name in chars, including the ending null
    static const uint32_t NameSize = 16;
    
    // shm queue size, must be a power of 2
    static const uint32_t ShmQueueSize = 2048;

    // set to the endian of majority of the hosts, e.g. true for x86
    static const bool ToLittleEndian = true; 

    // tcp send queue size, must be a multiple of 8
    static const uint32_t TcpQueueSize = 2000; 

    // tcp recv buff init size(recv buffer is allocated when tcp connection is established), must be a multiple of 8
    static const uint32_t TcpRecvBufInitSize = 2000;

    // tcp recv buff max size(recv buffer can expand when needed), must be a multiple of 8
    static const uint32_t TcpRecvBufMaxSize = 8000;

    // if enable TCP_NODELAY
    static const bool TcpNoDelay = true;

    // tcp connection timeout, measured in user provided timestamp
    static const int64_t ConnectionTimeout = 10;

    // delay of heartbeat msg after the last tcp msg send time, measured in user provided timestamp
    static const int64_t HeartBeatInverval = 3;

    // user defined data in LoginMsg, e.g. username, password..., take care of the endian
    using LoginUserData = char;

    // user defined data in LoginRspMsg, take care of the endian
    using LoginRspUserData = char;

    // user defined data in TcpShmConnection class
    using ConnectionUserData = char;
};
```


## 客户端部分
tcpshm_client.h定义了模板类`TcpShmClient`，用户需要定义一个新的派生自`TcpShmClient`的类，并提供一个配置模板类，以及为TcpShmClient的构造函数提供客户端名称和ptcp文件夹名称。客户端名称与服务器名称结合使用来唯一标识一个连接，ptcp文件夹由框架用来持久化一些内部文件，包括tcp队列文件。

```c++
#include "tcpshm/tcpshm_client.h"

struct Conf
{
    // Connection related Conf
    ...
};

class MyClient;
using TSClient = tcpshm::TcpShmClient<MyClient, Conf>;

class MyClient : public TSClient 
{
public:
    MyClient(const std::string& ptcp_dir, const std::string& name)
        : TSClient(ptcp_dir, name)
...
```

然后用户可以调用Connect()登录到服务器：

```c++
    // connect and login to server, may block for a short time
    // return true if success
    bool Connect(bool use_shm, // if using shm to transfer application msg
                 const char* server_ipv4, // server ip
                 uint16_t server_port, // server port
                 const typename Conf::LoginUserData& login_user_data //user defined login data to be copied into LoginMsg
                );
```

如果登录成功，用户可以获取连接引用来发送消息：

```c++
    // get the connection reference which can be kept by user as long as TcpShmClient is not destructed
    Connection& GetConnection();
```

为了接收消息并保持连接活跃，用户需要频繁地轮询客户端。对于TCP模式，用户调用PollTcp()；对于共享内存模式，用户需要同时调用PollTcp()和PollShm()，可以从同一个线程或不同的线程调用，使用单独的线程有应用消息延迟更低的优势。

```c++
    // we need to PollTcp even if using shm
    // now is a user provided timestamp, used to measure ConnectionTimeout and HeartBeatInverval
    void PollTcp(int64_t now);

    // only for using shm
    void PollShm();
```

要停止客户端，只需调用Stop()
```c++
    // stop the connection and close files
    void Stop();
```

此外，用户需要定义一系列框架将调用的回调函数：
```c++
    // called within Connect()
    // reporting errors on connecting to the server
    void OnSystemError(const char* error_msg, int sys_errno);

    // called within Connect()
    // Login rejected by server
    void OnLoginReject(const LoginRspMsg* login_rsp);

    // called within Connect()
    // confirmation for login success
    // return timestamp of now
    int64_t OnLoginSuccess(const LoginRspMsg* login_rsp);

    // called within Connect()
    // server and client ptcp sequence number don't match, we need to fix it manually
    void OnSeqNumberMismatch(uint32_t local_ack_seq,
                             uint32_t local_seq_start,
                             uint32_t local_seq_end,
                             uint32_t remote_ack_seq,
                             uint32_t remote_seq_start,
                             uint32_t remote_seq_end);

    // called by APP thread
    // handle a new app msg from server
    void OnServerMsg(MsgHeader* header);

    // called by tcp thread
    // connection is closed
    void OnDisconnected(const char* reason, int sys_errno);
```

## 服务器部分
tcpshm_server.h定义了模板类`TcpShmServer`，与`TcpShmClient`类似，用户需要定义一个新的派生自`TcpShmServer`的类，并提供一个配置模板类，以及为TcpShmServer的构造函数提供服务器名称和ptcp文件夹名称：
```c++
#include "tcpshm/tcpshm_server.h"

struct Conf
{
    // Connection related Conf:
    ...

    // Server related Conf:

    // max number of unlogined tcp connection
    static const uint32_t MaxNewConnections = 5;

    // max number of shm connection per group
    static const uint32_t MaxShmConnsPerGrp = 4;

    // number of shm connection groups
    static const uint32_t MaxShmGrps = 1;

    // max number of tcp connections per group
    static const uint32_t MaxTcpConnsPerGrp = 4;
    
    // number of tcp connection groups
    static const uint32_t MaxTcpGrps = 1;

    // unlogined tcp connection timeout, measured in user provided timestamp
    static const int64_t NewConnectionTimeout = 3;
};

class MyServer;
using TSServer = TcpShmServer<MyServer, Conf>;

class MyServer : public TSServer
{
public:
    MyServer(const std::string& ptcp_dir, const std::string& name)
        : TSServer(ptcp_dir, name) 
...
```
用户通过Start()和Stop()启动和停止服务器：
```c++
    // start the server
    // return true if success
    bool Start(const char* listen_ipv4, uint16_t listen_port);
    
    void Stop();
```

服务器的一个重要特性是它允许用户自定义他们的线程模型。
它支持的最大线程数是MaxShmGrps + MaxTcpGrps + 1(用于控制线程)，在这种情况下，每个组由一个单独的线程服务。
在另一个极端情况下，用户可以只使用一个线程服务所有内容。
这个逻辑由用户如何在他的线程中调用轮询函数来控制。
服务器有3个轮询函数，它们可以从同一个或不同的线程调用：
```c++
    // poll control for handling new connections and keep shm connections alive
    void PollCtl(int64_t now);

    // poll tcp for serving tcp connections
    void PollTcp(int64_t now, int grpid);

    // poll shm for serving shm connections
    void PollShm(int grpid);
```

此外，用户需要定义一系列框架将调用的回调函数：
```c++
    // called with Start()
    // reporting errors on Starting the server
    void OnSystemError(const char* errno_msg, int sys_errno);

    // called by CTL thread
    // if accept the connection, set user_data in login_rsp and return grpid with respect to tcp or shm
    // else set error_msg in login_rsp if possible, and return -1
    // Note that even if we accept it here, there could be other errors on handling the login,
    // so we have to wait OnClientLogon for confirmation
    int OnNewConnection(const struct sockaddr_in& addr, const LoginMsg* login, LoginRspMsg* login_rsp);

    // called by CTL thread
    // ptcp or shm files can't be open or are corrupt
    void OnClientFileError(Connection& conn, const char* reason, int sys_errno);

    // called by CTL thread
    // server and client ptcp sequence number don't match, we need to fix it manually
    void OnSeqNumberMismatch(Connection& conn,
                             uint32_t local_ack_seq,
                             uint32_t local_seq_start,
                             uint32_t local_seq_end,
                             uint32_t remote_ack_seq,
                             uint32_t remote_seq_start,
                             uint32_t remote_seq_end);

    // called by CTL thread
    // confirmation for client logon
    void OnClientLogon(const struct sockaddr_in& addr, Connection& conn);

    // called by CTL thread
    // client is disconnected
    void OnClientDisconnected(Connection& conn, const char* reason, int sys_errno); 

    // called by APP thread
    void OnClientMsg(Connection& conn, MsgHeader* recv_header);
```
