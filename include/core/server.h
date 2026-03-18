#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include "http/http_parser.h"
#include "net/dynamic_buffer.h"
#include "net/epoll_poller.h"
#include "net/socket_utils.h"

namespace core {

// Server 负责当前阶段的服务端主流程。
// 到 Week2 Day1 为止，它的职责是：
// 1. 创建监听 socket
// 2. 把监听 socket 切成非阻塞
// 3. 创建 epoll 实例
// 4. 维护最小连接表
// 5. 接收连接并读取数据
// 6. 用动态缓冲区按 "\r\n\r\n" 切分完整请求头
// 7. 对完整请求头做最小 HTTP 解析和协议边界防御
// 8. 对合法 GET/POST 生成静态 200 OK 响应
// 9. 当一次 write() 写不完时，把剩余数据放进 outbuf，等待后续 EPOLLOUT 继续发送
// 10. 使用 ReadyQueue 继续处理“用户态 buffer 里已经有完整请求，但本轮达到处理上限”的连接
class Server {
public:
    // 构造函数：
    // 把监听地址、端口和 backlog 保存下来，
    // 真正创建 socket 的动作放到 Initialize() 里做。
    Server(std::string host, std::uint16_t port, int backlog);
    // 析构函数使用默认实现即可；
    // 因为内部 fd 都交给 RAII 对象管理。
    ~Server() = default;

    // 禁止拷贝，避免 fd 和连接状态被多个对象重复管理。
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // 完成服务启动前的初始化：
    // 创建监听 socket、切非阻塞、创建 epoll、注册监听 fd。
    void Initialize();
    // 进入运行态，直到收到退出信号。
    void Run();

    // 返回当前监听 fd，主要用于调试和排错。
    [[nodiscard]] int listening_fd() const noexcept;

private:
    // ClientConnection 保存一个客户端连接在当前阶段所需的最小状态。
    struct ClientConnection {
        // 客户端 socket fd，由 RAII 负责自动关闭。
        net::UniqueFd socket;
        // 对端地址字符串，主要用于日志输出。
        std::string peer_endpoint;
        // 应用层输入缓冲区。
        // 这里会暂存多次 read() 收到的数据，直到能够按 "\r\n\r\n" 切出完整请求头。
        net::DynamicBuffer input_buffer;
        // 输出缓冲区保存“一次 write() 没写完”的响应尾部。
        // 只有在非阻塞 write 遇到 EAGAIN 或只写出一部分数据时，才会把剩余字节放到这里。
        std::string output_buffer;
        // 当前阶段正常响应仍然走 "Connection: close"。
        // 但为了支持一条连接里连续处理多个已缓冲请求，
        // 这里会等当前已经排队好的响应都写完之后再真正关闭连接。
        bool close_after_write = false;
        // [Week2 Day1] New: 标记该连接是否已经在 ReadyQueue 中，避免重复入队。
        bool in_ready_queue = false;
    };

    // 把监听 socket 设置为非阻塞。
    void MakeListenSocketNonBlocking() const;
    // 创建并初始化 epoll。
    void InitializePoller();
    // 把监听 fd 注册进 epoll，监听可读事件。
    void RegisterListenSocket();
    // 运行一轮 epoll 事件循环。
    void RunEventLoopOnce();
    // 处理一轮 ReadyQueue。
    // ReadyQueue 用来承接“用户态 buffer 中已有完整请求，但本轮不再继续处理”的连接。
    void ProcessReadyQueue();
    // 按事件来源分发处理逻辑。
    void HandlePollEvent(const net::PollEvent& event);
    // 处理监听 fd 上的可读事件。
    // 对监听 socket 来说，“可读”意味着有新连接可以 accept。
    void HandleListenEvent(std::uint32_t events);
    // 处理客户端连接上的事件。
    // 到 Week2 Day1 为止，这里主要区分三类情况：
    // 1. 对端关闭 / 错误
    // 2. 可读事件：继续收 header 并解析
    // 3. 可写事件：继续把 outbuf 中没写完的响应刷出去
    void HandleClientEvent(int fd, std::uint32_t events);

    // 循环 accept 新连接，直到当前没有更多连接可接。
    // 返回本轮成功接收的连接数量，便于日志统计。
    std::size_t DrainAcceptQueue();
    // 把一个新连接加入连接表并注册到 epoll。
    void RegisterAcceptedConnection(net::AcceptedSocket accepted);
    // 从一个客户端连接上循环读取数据，直到 EAGAIN 或连接关闭。
    void ReadFromClient(int fd);
    // 处理某个连接输入缓冲区里已经完整的请求头。
    // 返回值语义：
    // - true：当前轮还可以继续处理这个连接
    // - false：本轮应当停止处理，可能是连接已关闭，也可能是已达到单轮上限并入了 ReadyQueue
    bool ProcessBufferedHeaders(int fd);
    // 当还没找到 "\r\n\r\n" 时，检查请求头是否已经超过 8192 字节。
    bool CheckHeaderLimit(int fd);
    // 记录一条成功解析的请求摘要，方便 review 时观察解析结果。
    void LogParsedRequest(const ClientConnection& connection, const http::HttpRequest& request) const;
    // 对一个已经通过 Day5 校验的合法请求，启动当前阶段的静态 200 OK 响应流程。
    void StartOkResponse(int fd, const http::HttpRequest& request);
    // 继续把 outbuf 里还没发完的数据刷到 socket。
    void FlushClientOutput(int fd);
    // [Week2 Day1] New: 把一个连接加入 ReadyQueue，等待后续轮次继续处理。
    void EnqueueReadyConnection(int fd);
    // 根据当前连接状态更新 epoll 监听掩码。
    void UpdateClientPollMask(int fd);
    // 尝试写出一个小的错误响应，然后关闭连接。
    void SendErrorResponseAndClose(int fd,
                                   int status_code,
                                   const char* reason_phrase,
                                   const char* close_reason);
    // 关闭并移除一个客户端连接。
    void CloseClientConnection(int fd, const char* reason);

    std::string host_;
    std::uint16_t port_;
    int backlog_;

    // 监听 fd 由 RAII 封装托管，避免异常路径泄漏。
    net::UniqueFd listen_fd_;
    // epoll 封装对象。当前管理监听 fd 和客户端 fd。
    net::EpollPoller poller_;

    // [Week2 Day1] New: ReadyQueue 保存“已经达到单轮处理上限，但 buffer 里还有完整请求”的连接 fd。
    // 这些连接必须在后续轮次继续处理，不能依赖下一次 EPOLLIN。
    std::deque<int> ready_queue_;
    // 保存所有当前活跃的客户端连接。
    std::unordered_map<int, ClientConnection> clients_;

    // Day5 引入的最小 HTTP 头解析器。
    http::HttpParser http_parser_;
};

}  // namespace core
