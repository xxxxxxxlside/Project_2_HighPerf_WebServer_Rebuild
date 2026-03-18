#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <list>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "http/http_parser.h"
#include "net/dynamic_buffer.h"
#include "net/epoll_poller.h"
#include "net/socket_utils.h"

namespace core {

// Server 负责当前阶段的服务端主流程。
// 到 Week3 Day2 为止，它的职责是：
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
// 11. 对单连接单轮读/写量加预算，避免一个连接长时间独占 EventLoop
// 12. 使用唯一关闭入口和 pending_close_queue，避免关闭过程中的 UAF 风险
// 13. 对 inbuf/outbuf 维护全局 inflight budget，并对过大 Content-Length 返回 413
// 14. 在 accept 路径上增加按 IP 计数的 token bucket，限制瞬时接入速率
// 15. 对全局活跃连接数做 max_conns 封顶，超限连接仍然走统一关闭流程
// 16. 使用最小堆定时器，给连接挂载 header_timeout、body_timeout、idle_keepalive_timeout
// 17. 在解析完 header 后，按 Content-Length 驱动最小 body 接收状态机，并给慢速 body 设置 deadline
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
    // EventBudget 表示“当前这一轮”允许一个连接继续消耗的读/写预算。
    // Week2 Day2 的核心就是：
    // - EPOLLIN / ReadyQueue 续读时，最多只读 256KB
    // - EPOLLOUT / 响应写回时，最多只写 256KB
    struct EventBudget {
        std::size_t read_remaining = 0;
        std::size_t write_remaining = 0;
    };

    enum class TimerKind {
        kHeader,
        kBody,
        kIdleKeepAlive,
    };

    struct TimerEntry {
        std::chrono::steady_clock::time_point deadline {};
        int fd = -1;
        std::uint64_t connection_id = 0;
        std::uint64_t generation = 0;
        TimerKind kind = TimerKind::kHeader;
    };

    struct TimerEntryLater {
        bool operator()(const TimerEntry& left, const TimerEntry& right) const noexcept {
            return left.deadline > right.deadline;
        }
    };

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
        // close_after_write=true 表示当前连接在把已经排队好的响应刷完后就应当关闭。
        // Week3 Day2 起，请求体已经能被最小状态机读完，因此 keep-alive 不再局限于“无 body 请求”。
        bool close_after_write = false;
        // 标记该连接是否已经在 ReadyQueue 中，避免重复入队。
        bool in_ready_queue = false;
        // 标记这个连接是否因为“读预算耗尽”而需要在 ReadyQueue 中继续读。
        // 这个标记的存在，是为了区分两种 ReadyQueue 来源：
        // 1. buffer 里还有完整请求，需要继续 parse/process
        // 2. 还没读到完整请求，但这一轮的 read 预算已经用完，需要下一轮继续 read
        bool ready_for_read = false;
        // closing=true 表示该连接已经进入唯一关闭流程。
        // 一旦置位，后续任何读写/回调都应该跳过，避免对“正在销毁”的连接继续操作。
        bool closing = false;
        // connection_id_ 用来给每一条连接生命周期分配唯一编号。
        // 定时器只会作用到“fd 和 connection_id 都匹配”的连接，避免 fd 被内核复用后误伤新连接。
        std::uint64_t connection_id = 0;
        // keep_alive_enabled=true 表示当前连接完成本次响应后不会立刻关闭，
        // 可以继续等待下一条请求并接受 idle_keepalive_timeout 管理。
        bool keep_alive_enabled = false;
        // waiting_for_request_body=true 表示：
        // 1. 这个连接已经解析完一个合法 header
        // 2. 但它声明的 body 还没有收满
        // 3. 因此当前不能继续解析后续 header，必须先把这次 body 消费完
        bool waiting_for_request_body = false;
        // pending_request 保存“header 已合法解析，但 body 还没收完”的那条请求。
        // body 收满之后，会基于这份请求对象继续走现有的 200 OK 响应流程。
        http::HttpRequest pending_request;
        std::size_t pending_body_bytes_total = 0;
        std::size_t pending_body_bytes_remaining = 0;
        // header_timeout_armed / body_timeout_armed / idle_timeout_armed 表示当前是否真的挂着对应定时器。
        // generation 每次 arm/disarm 都会递增，用来让旧的堆节点自动失效。
        bool header_timeout_armed = false;
        std::uint64_t header_timeout_generation = 0;
        bool body_timeout_armed = false;
        std::uint64_t body_timeout_generation = 0;
        bool idle_timeout_armed = false;
        std::uint64_t idle_timeout_generation = 0;
    };

    // PendingCloseEntry 保存一个已经完成“摘 epoll + 关 fd + 清映射”的连接对象。
    // 这些对象不会立刻析构，而是在本轮 EventLoop 末尾统一释放。
    struct PendingCloseEntry {
        ClientConnection connection;
    };

    // IpTokenBucketEntry 保存一个来源 IP 当前的限流状态。
    // 当前阶段只在 accept 路径上使用它，所以结构里只保留最小字段：
    // 1. tokens：当前可用 token 数
    // 2. last_refill：上次补充 token 的时间
    // 3. last_seen：上次看到这个 IP 的时间，用于 TTL/LRU
    // 4. lru_iterator：指向 LRU 链表中的位置，方便 O(1) 更新
    struct IpTokenBucketEntry {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point last_refill {};
        std::chrono::steady_clock::time_point last_seen {};
        std::list<std::string>::iterator lru_iterator;
    };

    // 把监听 socket 设置为非阻塞。
    void MakeListenSocketNonBlocking() const;
    // 创建并初始化 epoll。
    void InitializePoller();
    // 把监听 fd 注册进 epoll，监听可读事件。
    void RegisterListenSocket();
    // 运行一轮 epoll 事件循环。
    void RunEventLoopOnce();
    // [Week3 Day2] New:
    // 处理最小堆里已经到期的超时任务。
    // 当前阶段实现三类超时：
    // 1. header_timeout：连接建立后，必须在 10 秒内收齐完整请求头
    // 2. body_timeout：header 合法后，必须在 deadline 内收齐请求体
    // 3. idle_keepalive_timeout：keep-alive 空闲连接最多保留 60 秒
    void ProcessExpiredTimers();
    // 根据 ReadyQueue 和最早一个定时器的 deadline，计算本轮 epoll_wait 最多阻塞多久。
    int ComputePollTimeoutMs() const;
    // 在本轮 EventLoop 末尾统一释放 pending_close_queue 中的连接对象。
    void FlushPendingCloseQueue();
    // 处理一轮 ReadyQueue。
    // ReadyQueue 用来承接“用户态 buffer 中已有完整请求，但本轮不再继续处理”的连接。
    void ProcessReadyQueue();
    // 处理一个来自 ReadyQueue 的连接。
    // 这个入口会按当前连接状态决定：
    // - 是继续刷 outbuf
    // - 还是继续处理已缓冲的完整请求
    // - 还是继续执行上轮被预算打断的 read
    void ContinueReadyConnection(int fd, EventBudget* budget);
    // 按事件来源分发处理逻辑。
    void HandlePollEvent(const net::PollEvent& event);
    // 处理监听 fd 上的可读事件。
    // 对监听 socket 来说，“可读”意味着有新连接可以 accept。
    void HandleListenEvent(std::uint32_t events);
    // 处理客户端连接上的事件。
    // 到 Week2 Day5 为止，这里主要区分三类情况：
    // 1. 对端关闭 / 错误
    // 2. 可读事件：继续收 header 并解析
    // 3. 可写事件：继续把 outbuf 中没写完的响应刷出去
    void HandleClientEvent(int fd, std::uint32_t events);

    // 循环 accept 新连接，直到当前没有更多连接可接。
    // 返回本轮成功接收的连接数量，便于日志统计。
    std::size_t DrainAcceptQueue();
    // 把一个新连接加入连接表并注册到 epoll。
    void RegisterAcceptedConnection(net::AcceptedSocket accepted);
    // [Week3 Day2] Begin:
    // accept 成功并注册后，立刻检查当前活跃连接数是否已经超过上限。
    // 这里单独拆成函数，是为了把“连接数封顶”的逻辑和“普通注册流程”分开：
    // 1. RegisterAcceptedConnection() 只负责把连接纳入管理
    // 2. 这个函数专门负责 Day6 的超限判定和拒绝语义
    //
    // 返回值语义：
    // - true ：连接数量仍在允许范围内，这个连接可以继续正常服务
    // - false：连接已经因为超限被拒绝，并且走完了统一关闭流程
    bool EnforceConnectionLimitAfterRegister(int fd);
    // [Week3 Day2] End
    // 从一个客户端连接上循环读取数据，直到 EAGAIN 或连接关闭。
    void ReadFromClient(int fd, EventBudget* budget);
    // 当一个连接已经进入“等待 body 收齐”的阶段时，尽量多消费当前 input buffer 里的 body 字节。
    // 返回 false 表示连接已经被关闭或本轮必须停止。
    bool ConsumePendingRequestBody(int fd, EventBudget* budget);
    // header 解析成功后，如果请求带有非零 Content-Length，就把连接切进 body 接收状态。
    // 返回 false 表示切换状态或后续处理过程中连接已经关闭。
    bool BeginRequestBodyReceive(int fd,
                                 const http::HttpRequest& request,
                                 EventBudget* budget);
    // 处理某个连接输入缓冲区里已经完整的请求头。
    // 返回值语义：
    // - true：当前轮还可以继续处理这个连接
    // - false：本轮应当停止处理，可能是连接已关闭，也可能是已达到单轮上限并入了 ReadyQueue
    bool ProcessBufferedHeaders(int fd, EventBudget* budget);
    // 当还没找到 "\r\n\r\n" 时，检查请求头是否已经超过 8192 字节。
    bool CheckHeaderLimit(int fd);
    // 记录一条成功解析的请求摘要，方便 review 时观察解析结果。
    void LogParsedRequest(const ClientConnection& connection, const http::HttpRequest& request) const;
    // 对一个已经通过 Day5 校验的合法请求，启动当前阶段的静态 200 OK 响应流程。
    void StartOkResponse(int fd, const http::HttpRequest& request, EventBudget* budget);
    // 继续把 outbuf 里还没发完的数据刷到 socket。
    void FlushClientOutput(int fd, EventBudget* budget);
    // 把一个连接加入 ReadyQueue，等待后续轮次继续处理。
    void EnqueueReadyConnection(int fd, const char* reason);
    // 根据当前连接状态更新 epoll 监听掩码。
    void UpdateClientPollMask(int fd);
    // [Week3 Day2] New:
    // 在连接开始等待请求头时挂载 header_timeout。
    // 这个状态既覆盖“刚 accept 进来还没收到首包”，也覆盖“keep-alive 连接已经开始下一条请求头，但迟迟收不完整”。
    void ArmHeaderTimeout(int fd, std::chrono::steady_clock::time_point now);
    // 在请求头已经收齐、并且开始等待 body 剩余字节时挂载 body_timeout。
    // deadline 口径遵守文档：min(body_timeout, body_deadline)。
    void ArmBodyTimeout(int fd,
                        std::chrono::steady_clock::time_point now,
                        std::size_t content_length);
    // 在连接进入 keep-alive 空闲态时挂载 idle_keepalive_timeout。
    void ArmIdleKeepAliveTimeout(int fd, std::chrono::steady_clock::time_point now);
    // 下面两个函数用于显式取消对应的超时任务。
    // 它们不需要从堆里物理删除节点，只会通过 generation 让旧节点在弹出时自动失效。
    void DisarmHeaderTimeout(ClientConnection* connection);
    void DisarmBodyTimeout(ClientConnection* connection);
    void DisarmIdleKeepAliveTimeout(ClientConnection* connection);
    // 判断一个请求在当前阶段能否安全保留 keep-alive。
    // 这里继续遵守现有结构，不引入新的路由或业务逻辑，只根据协议语义决定是否允许复用连接：
    // - HTTP/1.1 默认 keep-alive，除非 Connection: close
    // - HTTP/1.0 默认 close，只有显式 keep-alive 才保留
    bool ShouldKeepAlive(const http::HttpRequest& request) const;
    // 在向 inbuf/outbuf 追加数据前先做全局 inflight budget 检查。
    // 这一步只负责判定“能不能继续追加”，真正的计数递增只会发生在追加成功之后。
    bool TryReserveInflightBytes(int fd, std::size_t add_bytes, const char* reason);
    // 按“已释放的 buffer 字节数”回收全局 inflight budget。
    void ReleaseInflightBytes(std::size_t release_bytes) noexcept;
    // 带 inflight budget 检查地向输入缓冲区追加数据。
    bool AppendToInputBuffer(int fd, ClientConnection* connection, const char* data, std::size_t size);
    // 带 inflight budget 检查地向输出缓冲区尾部追加数据。
    bool AppendToOutputBuffer(int fd, ClientConnection* connection, std::string_view data);
    // 带 inflight budget 检查地替换整个输出缓冲区。
    bool ReplaceOutputBuffer(int fd, ClientConnection* connection, std::string_view data);
    // 在 accept 成功后、正式注册进 epoll 前，检查该 IP 是否还有可用 token。
    // 返回 false 表示当前连接已经被 429 + close 拒绝，不应再继续注册。
    bool CheckIpRateLimitBeforeRegister(const net::AcceptedSocket& accepted);
    // 按 elapsed time 给一个 IP bucket 补充 token。
    void RefillIpBucket(IpTokenBucketEntry* bucket, std::chrono::steady_clock::time_point now);
    // 把一个 bucket 更新成“最近访问”，维持 LRU 顺序。
    void TouchIpBucket(const std::string& ip,
                       IpTokenBucketEntry* bucket,
                       std::chrono::steady_clock::time_point now);
    // 清理超过 TTL 的旧 bucket，避免不同 IP 无限增长导致额外内存占用。
    void CleanupExpiredIpBuckets(std::chrono::steady_clock::time_point now);
    // 当 bucket 总数到达上限时，按 LRU 淘汰最旧条目。
    void EvictOldestIpBucket();
    // 尝试写出一个小的错误响应，然后关闭连接。
    void SendErrorResponseAndClose(int fd,
                                   int status_code,
                                   const char* reason_phrase,
                                   const char* close_reason);
    // 连接关闭的唯一入口。
    // 调用它时会统一完成：
    // 1. 置 closing=true
    // 2. 从 ReadyQueue 摘除
    // 3. epoll_ctl(DEL)
    // 4. close(fd)
    // 5. 清理 fd -> connection 映射
    // 6. 把对象放进 pending_close_queue，等待本轮末尾再真正释放
    void CloseClientConnection(int fd, const char* reason);

    std::string host_;
    std::uint16_t port_;
    int backlog_;

    // 监听 fd 由 RAII 封装托管，避免异常路径泄漏。
    net::UniqueFd listen_fd_;
    // epoll 封装对象。当前管理监听 fd 和客户端 fd。
    net::EpollPoller poller_;

    // ReadyQueue 保存“本轮不能继续处理，但后续轮次还必须继续”的连接 fd。
    // 这些连接必须在后续轮次继续处理，不能依赖下一次 EPOLLIN。
    std::deque<int> ready_queue_;
    // pending_close_queue 保存“本轮已关闭但尚未真正释放”的连接对象。
    std::vector<PendingCloseEntry> pending_close_queue_;
    // timer_heap_ 是当前阶段的最小堆定时器。
    // 它只保存最小必要信息，由 EventLoop 线程串行 push/pop。
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerEntryLater> timer_heap_;
    // 保存所有当前活跃的客户端连接。
    std::unordered_map<int, ClientConnection> clients_;
    // [Week3 Day2] New:
    // active_connection_count_ 只统计“已经纳入 epoll 管理，且 closing=false”的连接数。
    // 它的口径和文档里的 max_conns 完全一致：
    // 1. accept 成功并完成注册时 +1
    // 2. 首次进入唯一关闭流程、置 closing=true 时 -1
    //
    // 之所以不用 clients_.size() 临时现算，是为了把“什么时候加、什么时候减”的语义写死，
    // 后面做 review 或加 metrics 时更容易检查这条计数是不是守恒。
    std::size_t active_connection_count_ = 0;
    // conn_reject_total_ 统计“因为超过 max_conns 而被拒绝”的连接总数。
    // 当前阶段它主要用于日志和 review，Week3 再继续接到统一 metrics。
    std::size_t conn_reject_total_ = 0;
    // next_connection_id_ 为每条连接生命周期分配一个单调递增编号。
    std::uint64_t next_connection_id_ = 1;
    // 统计当前所有活跃连接 inbuf + outbuf 占用的总字节数。
    std::size_t global_inflight_bytes_ = 0;
    // ip_bucket_lru_ + ip_token_buckets_ 共同维护“有界、可过期”的按 IP 限流状态。
    std::list<std::string> ip_bucket_lru_;
    std::unordered_map<std::string, IpTokenBucketEntry> ip_token_buckets_;

    // Day5 引入的最小 HTTP 头解析器。
    http::HttpParser http_parser_;
};

}  // namespace core
