#include "core/server.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <string_view>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>

#include "http/http_response.h"
#include "net/socket_utils.h"

namespace core {

namespace {

// 这个辅助函数专门读取全局退出标记。
// Server::Run() 会轮询它，决定是否结束事件循环。
bool StopRequested() {
    return net::GlobalStopFlag() != 0;
}

// 每次 read() 读一个固定大小的小块。
// 这个大小不是协议限制，只是当前阶段每次从内核取数据的临时缓冲区大小。
constexpr std::size_t kReadChunkSize = 4096;

// Day5 文档里明确给出的 header 上限。
constexpr std::size_t kMaxHeaderBytes = 8192;

// 当前阶段先继续用静态文本把正常响应链路走通。
constexpr std::string_view kStaticOkBody = "RouteC MVP response\n";

// Week2 Day1 要求单连接单轮最多只处理 5 个完整请求。
// 这样做的目的是避免一个连接持续占用 EventLoop，导致其它连接饥饿。
constexpr std::size_t kMaxRequestsPerRound = 5;

// Week2 Day2 新增单轮读/写预算。
// 这两个预算都按“单连接、单轮处理”计算，而不是全局共享。
constexpr std::size_t kMaxReadBytesPerEvent = 256 * 1024;
constexpr std::size_t kMaxWriteBytesPerEvent = 256 * 1024;

// Week2 Day4 引入两个新的硬限制：
// 1. Content-Length 不能超过 8MB
// 2. 所有活跃连接 inbuf + outbuf 的总和不能超过 512MB
constexpr std::size_t kMaxBodyBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxInflightBytes = 512ULL * 1024 * 1024;

// Week2 Day5 在 accept 路径上加入按 IP 的 token bucket。
constexpr double kIpBucketBurst = 200.0;
constexpr double kIpBucketRefillPerSecond = 50.0;
constexpr std::size_t kIpBucketMaxEntries = 100000;
constexpr auto kIpBucketTtl = std::chrono::minutes(10);

// Week2 Day6 新增全局活跃连接数封顶。
// 这里写死默认值 200k，和文档保持一致；后续如果项目再引入配置系统，再把它接成可配置项。
constexpr std::size_t kMaxConnections = 200000;

// Week3 Day2 继续扩展超时基础：
// 1. header_timeout：收 header 的最大等待时间
// 2. body_timeout：收 body 的硬上限
// 3. idle_keepalive_timeout：keep-alive 空闲连接超时
constexpr auto kHeaderTimeout = std::chrono::seconds(10);
constexpr auto kBodyTimeout = std::chrono::seconds(120);
constexpr auto kIdleKeepAliveTimeout = std::chrono::seconds(60);
constexpr auto kMetricsLogInterval = std::chrono::seconds(1);

// 当前工程到这里仍然只支持 IPv4，对端地址字符串始终是 "ip:port"。
// 因此这里直接按最后一个 ':' 把 IP 部分切出来即可。
std::string ExtractIpFromPeerEndpoint(std::string_view peer_endpoint) {
    const std::size_t colon = peer_endpoint.rfind(':');
    if (colon == std::string_view::npos) {
        return std::string(peer_endpoint);
    }
    return std::string(peer_endpoint.substr(0, colon));
}

std::string LowercaseCopy(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char ch : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool ContainsHeaderToken(std::string_view header_value, std::string_view token) {
    return LowercaseCopy(header_value).find(std::string(token)) != std::string::npos;
}

std::chrono::steady_clock::time_point CalculateBodyReceiveDeadline(
    std::chrono::steady_clock::time_point now,
    std::size_t content_length) {
    // 文档口径：
    // body_deadline = now + min(120s, max(30s, Content-Length / 65536Bps))
    const double raw_seconds = static_cast<double>(content_length) / 65536.0;
    const double bounded_seconds = std::min(120.0, std::max(30.0, raw_seconds));
    const auto body_deadline =
        now + std::chrono::milliseconds(static_cast<long long>(bounded_seconds * 1000.0));
    const auto hard_timeout_deadline = now + kBodyTimeout;
    return std::min(body_deadline, hard_timeout_deadline);
}

std::string SanitizeMachineName(std::string text) {
    for (char& ch : text) {
        const bool allowed =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_';
        if (!allowed) {
            ch = '_';
        }
    }
    return text;
}

std::string BuildMetricsRunDirectoryName() {
    char hostname[256] = {};
    std::string machine = "unknown";
    if (::gethostname(hostname, sizeof(hostname) - 1) == 0 && hostname[0] != '\0') {
        machine = hostname;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm {};
    localtime_r(&now_time, &local_tm);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y%m%d") << '_' << SanitizeMachineName(machine);
    return oss.str();
}

}  // namespace

// 构造函数只保存配置，不做系统调用。
Server::Server(std::string host, std::uint16_t port, int backlog)
    : host_(std::move(host)), port_(port), backlog_(backlog) {}

// Initialize() 负责完成服务启动前的准备工作。
// 到 Week3 Day3 为止，这里在监听/epoll 初始化之外，还会准备 metrics 输出文件。
void Server::Initialize() {
    listen_fd_ = net::CreateListenSocket(host_, port_, backlog_);
    MakeListenSocketNonBlocking();
    InitializePoller();
    InitializeMetricsLogging();
    RegisterListenSocket();
}

// Run() 会一直阻塞在事件循环里，直到外部发来退出信号。
void Server::Run() {
    if (!listen_fd_.valid() || !poller_.IsOpen()) {
        throw std::logic_error("server must be initialized before run()");
    }

    std::cout
        << "[Week3 Day2] listening on "
        << net::DescribeEndpoint(host_, port_)
        << ", backlog=" << backlog_
        << ", non-blocking=true"
        << ", epoll=LT"
        << ", metrics_log=" << metrics_log_path_
        << '\n';

    while (!StopRequested()) {
        RunEventLoopOnce();
    }

    MaybeWriteMetricsLog();
}

// 把监听 fd 切成非阻塞。
void Server::MakeListenSocketNonBlocking() const {
    net::SetNonBlocking(listen_fd_.get());
}

// 创建 epoll 实例。
void Server::InitializePoller() {
    poller_.Open();
}

void Server::InitializeMetricsLogging() {
    // Week3 Day3 先把 metrics 落盘路径固定为 results/webserver/<date>_<machine>/metrics.log。
    // 这里还不做 Day6 的完整证据目录，只准备当前运行需要的 metrics.log。
    const std::filesystem::path metrics_dir =
        std::filesystem::current_path() / "results" / "webserver" / BuildMetricsRunDirectoryName();
    std::filesystem::create_directories(metrics_dir);

    metrics_log_path_ = (metrics_dir / "metrics.log").string();
    metrics_log_stream_.open(metrics_log_path_, std::ios::out | std::ios::trunc);
    if (!metrics_log_stream_.is_open()) {
        throw std::runtime_error("failed to open metrics log: " + metrics_log_path_);
    }

    metrics_start_time_ = std::chrono::steady_clock::now();
    next_metrics_log_time_ = metrics_start_time_ + kMetricsLogInterval;
}

void Server::MaybeWriteMetricsLog() {
    if (!metrics_log_stream_.is_open()) {
        return;
    }

    const auto now_steady = std::chrono::steady_clock::now();
    if (now_steady < next_metrics_log_time_) {
        return;
    }

    const auto now_system = std::chrono::system_clock::now();
    const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now_system.time_since_epoch());
    const auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
        now_steady - metrics_start_time_);

    metrics_log_stream_
        << "ts=" << unix_ms.count()
        << " uptime_s=" << uptime_s.count()
        << " rss_kb=" << ReadCurrentRssKb()
        << " errors_total=" << metrics_errors_total_.load(std::memory_order_relaxed)
        << " accept_total=" << metrics_accept_total_.load(std::memory_order_relaxed)
        << " conns_current=" << metrics_conns_current_.load(std::memory_order_relaxed)
        << " requests_total=" << metrics_requests_total_.load(std::memory_order_relaxed)
        << " reject_total=" << metrics_reject_total_.load(std::memory_order_relaxed)
        << " reject_411_total=" << metrics_reject_411_total_.load(std::memory_order_relaxed)
        << " reject_413_total=" << metrics_reject_413_total_.load(std::memory_order_relaxed)
        << " reject_429_total=" << metrics_reject_429_total_.load(std::memory_order_relaxed)
        << " reject_431_total=" << metrics_reject_431_total_.load(std::memory_order_relaxed)
        << " reject_501_total=" << metrics_reject_501_total_.load(std::memory_order_relaxed)
        << " reject_503_total=" << metrics_reject_503_total_.load(std::memory_order_relaxed)
        << " conn_reject_total=" << metrics_conn_reject_total_.load(std::memory_order_relaxed)
        << " timeout_close_total=" << metrics_timeout_close_total_.load(std::memory_order_relaxed)
        << " global_inflight_bytes_current="
        << metrics_global_inflight_bytes_current_.load(std::memory_order_relaxed)
        << '\n';
    metrics_log_stream_.flush();

    do {
        next_metrics_log_time_ += kMetricsLogInterval;
    } while (next_metrics_log_time_ <= now_steady);
}

std::uint64_t Server::ReadCurrentRssKb() const {
    std::ifstream status_file("/proc/self/status");
    if (!status_file.is_open()) {
        return 0;
    }

    std::string line;
    while (std::getline(status_file, line)) {
        if (line.rfind("VmRSS:", 0) != 0) {
            continue;
        }

        unsigned long long rss_kb = 0;
        if (std::sscanf(line.c_str(), "VmRSS: %llu kB", &rss_kb) == 1) {
            return static_cast<std::uint64_t>(rss_kb);
        }
        return 0;
    }

    return 0;
}

void Server::RecordRejectMetric(int status_code) {
    metrics_reject_total_.fetch_add(1, std::memory_order_relaxed);
    switch (status_code) {
    case 411:
        metrics_reject_411_total_.fetch_add(1, std::memory_order_relaxed);
        break;
    case 413:
        metrics_reject_413_total_.fetch_add(1, std::memory_order_relaxed);
        break;
    case 429:
        metrics_reject_429_total_.fetch_add(1, std::memory_order_relaxed);
        break;
    case 431:
        metrics_reject_431_total_.fetch_add(1, std::memory_order_relaxed);
        break;
    case 501:
        metrics_reject_501_total_.fetch_add(1, std::memory_order_relaxed);
        break;
    case 503:
        metrics_reject_503_total_.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

void Server::RecordErrorMetric() {
    metrics_errors_total_.fetch_add(1, std::memory_order_relaxed);
}

// 把监听 fd 注册进 epoll。
void Server::RegisterListenSocket() {
    poller_.Add(listen_fd_.get(), EPOLLIN);
}

// 跑一轮 epoll_wait，然后把返回的事件逐个分发出去。
void Server::RunEventLoopOnce() {
    ProcessReadyQueue();
    ProcessExpiredTimers();
    MaybeWriteMetricsLog();

    // ReadyQueue 和最早一个超时任务会共同决定 epoll_wait 最多能阻塞多久。
    // Week3 Day3 起，这里还要兼顾 metrics 的每秒落盘时刻。
    const int wait_timeout_ms = ComputePollTimeoutMs();
    const std::vector<net::PollEvent> events = poller_.Wait(wait_timeout_ms);

    for (const net::PollEvent& event : events) {
        HandlePollEvent(event);
    }

    ProcessExpiredTimers();
    MaybeWriteMetricsLog();
    FlushPendingCloseQueue();
}

void Server::ProcessExpiredTimers() {
    const auto now = std::chrono::steady_clock::now();

    while (!timer_heap_.empty() && timer_heap_.top().deadline <= now) {
        const TimerEntry timer = timer_heap_.top();
        timer_heap_.pop();

        auto it = clients_.find(timer.fd);
        if (it == clients_.end() || it->second.closing) {
            continue;
        }

        ClientConnection& connection = it->second;
        if (connection.connection_id != timer.connection_id) {
            continue;
        }

        if (timer.kind == TimerKind::kHeader) {
            if (!connection.header_timeout_armed ||
                connection.header_timeout_generation != timer.generation) {
                continue;
            }
            connection.header_timeout_armed = false;
            ++connection.header_timeout_generation;
        } else if (timer.kind == TimerKind::kBody) {
            if (!connection.body_timeout_armed ||
                connection.body_timeout_generation != timer.generation) {
                continue;
            }
            connection.body_timeout_armed = false;
            ++connection.body_timeout_generation;
        } else {
            if (!connection.idle_timeout_armed ||
                connection.idle_timeout_generation != timer.generation) {
                continue;
            }
            connection.idle_timeout_armed = false;
            ++connection.idle_timeout_generation;
        }

        std::cout
            << "[Week3 Day2] timer expired for "
            << connection.peer_endpoint
            << ", kind=" << (timer.kind == TimerKind::kHeader
                                  ? "header_timeout"
                                  : (timer.kind == TimerKind::kBody
                                         ? "body_timeout"
                                         : "idle_keepalive_timeout"))
            << '\n';

        RecordErrorMetric();
        metrics_timeout_close_total_.fetch_add(1, std::memory_order_relaxed);
        if (timer.kind == TimerKind::kHeader) {
            CloseClientConnection(timer.fd, "header timeout exceeded before a complete request header arrived");
        } else if (timer.kind == TimerKind::kBody) {
            CloseClientConnection(timer.fd, "body timeout exceeded before request body was fully received");
        } else {
            CloseClientConnection(timer.fd, "idle keep-alive timeout exceeded");
        }
    }
}

int Server::ComputePollTimeoutMs() const {
    if (!ready_queue_.empty()) {
        return 0;
    }

    int wait_timeout_ms = 1000;
    if (timer_heap_.empty()) {
        wait_timeout_ms = 1000;
    } else {
        const auto now = std::chrono::steady_clock::now();
        const auto next_deadline = timer_heap_.top().deadline;
        if (next_deadline <= now) {
            return 0;
        }

        const auto wait =
            std::chrono::duration_cast<std::chrono::milliseconds>(next_deadline - now);
        if (wait.count() <= 0) {
            return 0;
        }

        wait_timeout_ms = wait.count() > 1000 ? 1000 : static_cast<int>(wait.count());
    }

    if (next_metrics_log_time_ == std::chrono::steady_clock::time_point {}) {
        return wait_timeout_ms;
    }

    const auto now = std::chrono::steady_clock::now();
    if (next_metrics_log_time_ <= now) {
        return 0;
    }

    const auto metrics_wait =
        std::chrono::duration_cast<std::chrono::milliseconds>(next_metrics_log_time_ - now);
    if (metrics_wait.count() <= 0) {
        return 0;
    }

    return std::min(wait_timeout_ms, static_cast<int>(metrics_wait.count()));
}

// 在本轮 EventLoop 末尾统一释放 pending_close_queue 中保存的连接对象。
// 这些对象在进入队列前，已经完成：
// 1. closing=true
// 2. 从 epoll 和 ReadyQueue 摘除
// 3. close(fd)
// 4. 从 clients_ 映射中删除
//
// 因此这里的职责很单纯：只负责真正释放对象内存。
void Server::FlushPendingCloseQueue() {
    if (pending_close_queue_.empty()) {
        return;
    }

    std::cout
        << "[Week3 Day2] releasing "
        << pending_close_queue_.size()
        << " connection object(s) from pending_close_queue"
        << '\n';

    pending_close_queue_.clear();
}

// 处理一轮 ReadyQueue。
// 这里故意只处理“本轮开始时队列中已有的元素”，不把新入队的连接继续在同一轮吃完，
// 这样可以把“下一轮再处理”的边界保持清晰，避免一个连接反复自我入队后独占 EventLoop。
void Server::ProcessReadyQueue() {
    const std::size_t ready_count = ready_queue_.size();
    for (std::size_t i = 0; i < ready_count; ++i) {
        const int fd = ready_queue_.front();
        ready_queue_.pop_front();

        auto it = clients_.find(fd);
        if (it == clients_.end() || it->second.closing) {
            continue;
        }

        it->second.in_ready_queue = false;
        EventBudget budget {kMaxReadBytesPerEvent, kMaxWriteBytesPerEvent};
        ContinueReadyConnection(fd, &budget);
    }
}

// 继续处理一个已经进入 ReadyQueue 的连接。
// 顺序上有意做成：
// 1. 先写：如果 outbuf 里还有响应尾部，优先刷完，避免响应顺序被打乱
// 2. 再消费 body：如果这个连接已经进入“等待 body 收齐”，就先继续消费 body
// 3. 再 parse：如果 input_buffer 里已经有完整请求，优先消费这些“已经在用户态可见”的工作
// 4. 最后读：如果上轮是因为读预算耗尽才停下，就继续 read
void Server::ContinueReadyConnection(int fd, EventBudget* budget) {
    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    if (!it->second.output_buffer.empty()) {
        FlushClientOutput(fd, budget);
    }

    it = clients_.find(fd);
    if (it == clients_.end() || !it->second.output_buffer.empty()) {
        return;
    }

    if (it->second.waiting_for_request_body) {
        if (!ConsumePendingRequestBody(fd, budget)) {
            return;
        }
    }

    it = clients_.find(fd);
    if (it == clients_.end() || it->second.waiting_for_request_body || !it->second.output_buffer.empty()) {
        return;
    }

    if (it->second.input_buffer.HasCompleteHeader()) {
        if (!ProcessBufferedHeaders(fd, budget)) {
            return;
        }
    }

    it = clients_.find(fd);
    if (it == clients_.end() || !it->second.output_buffer.empty()) {
        return;
    }

    if (it->second.ready_for_read) {
        it->second.ready_for_read = false;
        ReadFromClient(fd, budget);
    }
}

// 把一个连接加入 ReadyQueue。
// 加入条件只有一个：当前连接 buffer 中明明还有完整请求，但本轮不允许继续处理了。
void Server::EnqueueReadyConnection(int fd, const char* reason) {
    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    if (it->second.in_ready_queue) {
        return;
    }

    it->second.in_ready_queue = true;
    ready_queue_.push_back(fd);

    std::cout
        << "[Week3 Day2] queued client "
        << it->second.peer_endpoint
        << " into ReadyQueue, reason=" << reason
        << ", buffered=" << it->second.input_buffer.size()
        << '\n';
}

// 统一分发一轮事件。
void Server::HandlePollEvent(const net::PollEvent& event) {
    if (event.fd == listen_fd_.get()) {
        HandleListenEvent(event.events);
        return;
    }

    auto it = clients_.find(event.fd);
    if (it == clients_.end()) {
        return;
    }

    // closing=true 的连接已经进入唯一关闭流程。
    // 这时即使某些回调仍然拿到了旧 fd，也必须直接跳过，避免 UAF 或误处理。
    if (it->second.closing) {
        return;
    }

    HandleClientEvent(event.fd, event.events);
}

// 处理监听 fd 的事件。
void Server::HandleListenEvent(std::uint32_t events) {
    if ((events & EPOLLIN) == 0U) {
        return;
    }

    const std::size_t accepted_count = DrainAcceptQueue();
    if (accepted_count > 0) {
        std::cout
            << "[Week3 Day2] drained "
            << accepted_count
            << " connection(s) from listen queue"
            << '\n';
    }
}

// 处理客户端 fd 的事件。
// 到 Week3 Day2 为止，连接可能同时出现：
// 1. 读事件：继续接收请求头
// 2. body 接收中的读事件：继续把声明的 Content-Length 收满
// 3. 写事件：把 outbuf 里未发完的响应继续写出去
// 4. 关闭/错误事件：直接回收连接
void Server::HandleClientEvent(int fd, std::uint32_t events) {
    if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
        CloseClientConnection(fd, "peer hangup or error event");
        return;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    EventBudget budget {kMaxReadBytesPerEvent, kMaxWriteBytesPerEvent};

    // 如果当前连接已经进入“响应待写完”状态，就优先处理 EPOLLOUT。
    // 只有当待发送数据已经清空后，才继续考虑本轮的 EPOLLIN。
    if (!it->second.output_buffer.empty()) {
        if ((events & EPOLLOUT) != 0U) {
            FlushClientOutput(fd, &budget);
        }

        it = clients_.find(fd);
        if (it == clients_.end() || !it->second.output_buffer.empty()) {
            return;
        }
    }

    if ((events & EPOLLIN) != 0U) {
        if (it->second.idle_timeout_armed) {
            DisarmIdleKeepAliveTimeout(&it->second);
            ArmHeaderTimeout(fd, std::chrono::steady_clock::now());

            it = clients_.find(fd);
            if (it == clients_.end() || it->second.closing) {
                return;
            }
        }
        ReadFromClient(fd, &budget);
    }
}

// 循环 accept 新连接，直到当前没有更多连接可接。
std::size_t Server::DrainAcceptQueue() {
    std::size_t accepted_count = 0;

    while (true) {
        net::AcceptedSocket accepted;
        if (!net::TryAcceptOne(listen_fd_.get(), &accepted)) {
            break;
        }

        ++accepted_count;
        metrics_accept_total_.fetch_add(1, std::memory_order_relaxed);
        RegisterAcceptedConnection(std::move(accepted));
    }

    return accepted_count;
}

// 接到新连接后，把它纳入最小连接表，并注册到 epoll。
// 但在真正注册之前，要先过一遍按 IP 的 token bucket。
void Server::RegisterAcceptedConnection(net::AcceptedSocket accepted) {
    const int client_fd = accepted.fd.get();
    if (client_fd < 0) {
        throw std::logic_error("accepted socket must be valid");
    }

    // accept 成功不等于一定放行。
    // 当前 accept 路径上有两层守门：
    // 1. 先做 IP token bucket，避免单个来源在短时间内冲进太多连接
    // 2. 再做全局连接数封顶，避免总连接数继续上涨
    if (!CheckIpRateLimitBeforeRegister(accepted)) {
        return;
    }

    ClientConnection connection;
    connection.socket = std::move(accepted.fd);
    connection.peer_endpoint = std::move(accepted.peer_endpoint);
    connection.connection_id = next_connection_id_++;

    poller_.Add(client_fd, EPOLLIN | EPOLLRDHUP);
    clients_.emplace(client_fd, std::move(connection));
    ++active_connection_count_;
    metrics_conns_current_.store(active_connection_count_, std::memory_order_relaxed);
    ArmHeaderTimeout(client_fd, std::chrono::steady_clock::now());

    std::cout
        << "[Week3 Day2] registered client "
        << clients_.at(client_fd).peer_endpoint
        << ", fd=" << client_fd
        << ", active_connections=" << active_connection_count_
        << '\n';

    // 连接已经纳入 epoll 和 clients_ 之后，立刻检查全局连接数上限。
    // 这里故意放在“注册之后”而不是“注册之前”，是为了严格遵守文档里的计数口径：
    // 1. accept 成功并纳入管理后，活跃连接数先 +1
    // 2. 如果发现超限，再通过唯一关闭入口把 closing=true，并在那个入口里 -1
    //
    // 这样无论是正常关闭还是“刚 accept 就被拒绝”，都走同一套生命周期路径，
    // 后面做 review 时更容易检查连接数和拒绝计数是不是对得上。
    if (!EnforceConnectionLimitAfterRegister(client_fd)) {
        return;
    }
}

// 这个函数专门收口“accept 成功后发现总连接数超限”的分支。
// 它不直接手写 close 逻辑，而是继续复用唯一关闭入口，原因有两个：
// 1. 连接计数的递减口径已经写进关闭入口，避免一处忘减导致统计失真
// 2. 关闭顺序、ReadyQueue 摘除、pending_close_queue 延迟释放也都已经在那条路径里统一好了
bool Server::EnforceConnectionLimitAfterRegister(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return false;
    }

    if (active_connection_count_ <= kMaxConnections) {
        return true;
    }

    ++conn_reject_total_;
    metrics_conn_reject_total_.fetch_add(1, std::memory_order_relaxed);
    metrics_reject_total_.fetch_add(1, std::memory_order_relaxed);
    RecordErrorMetric();
    std::cout
        << "[Week3 Day2] rejecting client "
        << it->second.peer_endpoint
        << " because active connection limit was exceeded"
        << ", active_connections=" << active_connection_count_
        << ", max_connections=" << kMaxConnections
        << ", conn_reject_total=" << conn_reject_total_
        << '\n';

    CloseClientConnection(fd, "active connection limit exceeded after accept");
    return false;
}

// accept 路径上的 IP token bucket。

// 在 accept 成功后、真正注册连接前检查该 IP 是否还有 token。
// 设计上把这个检查放在“accept 之后、epoll 注册之前”，原因是：
// 1. token 的统计口径就是“accept 成功消耗 1 个 token”
// 2. 如果已经超限，就不应该再把这个连接纳入正常连接管理结构
bool Server::CheckIpRateLimitBeforeRegister(const net::AcceptedSocket& accepted) {
    const auto now = std::chrono::steady_clock::now();
    CleanupExpiredIpBuckets(now);

    const std::string ip = ExtractIpFromPeerEndpoint(accepted.peer_endpoint);
    auto it = ip_token_buckets_.find(ip);

    if (it == ip_token_buckets_.end()) {
        while (ip_token_buckets_.size() >= kIpBucketMaxEntries && !ip_bucket_lru_.empty()) {
            EvictOldestIpBucket();
        }

        ip_bucket_lru_.push_back(ip);

        IpTokenBucketEntry entry;
        entry.tokens = kIpBucketBurst;
        entry.last_refill = now;
        entry.last_seen = now;
        entry.lru_iterator = std::prev(ip_bucket_lru_.end());

        auto inserted = ip_token_buckets_.emplace(ip, std::move(entry));
        it = inserted.first;
    } else {
        RefillIpBucket(&it->second, now);
        TouchIpBucket(ip, &it->second, now);
    }

    // 口径要求是“每次 accept 成功消耗 1 个 token”。
    // 所以新建 bucket 即使初始化为 burst，也要在这里立刻扣掉本次 accept 的 1 个 token。
    if (it->second.tokens < 1.0) {
        const std::string response = http::BuildSimpleErrorResponse(
            429,
            "Too Many Requests",
            "Too Many Requests\n");

        RecordRejectMetric(429);
        RecordErrorMetric();
        const bool write_ok = net::WriteBestEffort(accepted.fd.get(), response);
        std::cout
            << "[Week3 Day2] rejected accepted socket from "
            << accepted.peer_endpoint
            << ", reason=ip token bucket exhausted"
            << ", write_complete=" << (write_ok ? "true" : "false")
            << '\n';
        return false;
    }

    it->second.tokens -= 1.0;
    return true;
}

// 根据距离上次 refill 的时间，按固定速率补充 token。
// 这里使用 double，是为了保留“小于 1 个 token”的小数补充量，避免限流呈现很硬的台阶效应。
void Server::RefillIpBucket(IpTokenBucketEntry* bucket, std::chrono::steady_clock::time_point now) {
    if (bucket == nullptr) {
        throw std::invalid_argument("bucket must not be null");
    }

    const std::chrono::duration<double> elapsed = now - bucket->last_refill;
    if (elapsed.count() <= 0.0) {
        return;
    }

    bucket->tokens = std::min(
        kIpBucketBurst,
        bucket->tokens + elapsed.count() * kIpBucketRefillPerSecond);
    bucket->last_refill = now;
}

// 访问一个已有 IP 条目时，要把它挪到 LRU 尾部，并更新 last_seen。
// 这样后续 TTL 清理和容量淘汰都能优先处理最久没出现过的 IP。
void Server::TouchIpBucket(const std::string& ip,
                           IpTokenBucketEntry* bucket,
                           std::chrono::steady_clock::time_point now) {
    if (bucket == nullptr) {
        throw std::invalid_argument("bucket must not be null");
    }

    ip_bucket_lru_.erase(bucket->lru_iterator);
    ip_bucket_lru_.push_back(ip);
    bucket->lru_iterator = std::prev(ip_bucket_lru_.end());
    bucket->last_seen = now;
}

// 由于 LRU 顺序本身就是按最近访问排序的，所以清理过期条目时只需要从队头开始扫。
// 一旦队头还没过期，后面的条目只会更新得更晚，也就可以立刻停止。
void Server::CleanupExpiredIpBuckets(std::chrono::steady_clock::time_point now) {
    while (!ip_bucket_lru_.empty()) {
        const std::string& oldest_ip = ip_bucket_lru_.front();
        auto it = ip_token_buckets_.find(oldest_ip);
        if (it == ip_token_buckets_.end()) {
            ip_bucket_lru_.pop_front();
            continue;
        }

        if (now - it->second.last_seen <= kIpBucketTtl) {
            return;
        }

        ip_bucket_lru_.pop_front();
        ip_token_buckets_.erase(it);
    }
}

// 当条目数达到上限时，按 LRU 淘汰最旧 IP。
void Server::EvictOldestIpBucket() {
    if (ip_bucket_lru_.empty()) {
        return;
    }

    const std::string oldest_ip = ip_bucket_lru_.front();
    ip_bucket_lru_.pop_front();
    ip_token_buckets_.erase(oldest_ip);
}

// inflight budget 统一在 Server 内部串行维护。

// 在向 inbuf/outbuf 追加数据前，先检查全局 inflight budget 是否还有空间。
// 这里故意只做“判定”，不直接递增计数；
// 因为文档要求只有在 append/assign 真正成功之后，global_inflight_bytes 才能增加。
bool Server::TryReserveInflightBytes(int fd, std::size_t add_bytes, const char* reason) {
    if (add_bytes == 0) {
        return true;
    }

    const bool over_limit =
        global_inflight_bytes_ > kMaxInflightBytes ||
        add_bytes > (kMaxInflightBytes - global_inflight_bytes_);

    if (!over_limit) {
        return true;
    }

    SendErrorResponseAndClose(fd,
                              503,
                              "Service Unavailable",
                              reason);
    return false;
}

void Server::ArmHeaderTimeout(int fd, std::chrono::steady_clock::time_point now) {
    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    ClientConnection& connection = it->second;
    DisarmIdleKeepAliveTimeout(&connection);
    connection.header_timeout_armed = true;
    ++connection.header_timeout_generation;
    timer_heap_.push(TimerEntry {
        now + kHeaderTimeout,
        fd,
        connection.connection_id,
        connection.header_timeout_generation,
        TimerKind::kHeader,
    });
}

void Server::ArmBodyTimeout(int fd,
                            std::chrono::steady_clock::time_point now,
                            std::size_t content_length) {
    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    ClientConnection& connection = it->second;
    DisarmHeaderTimeout(&connection);
    DisarmBodyTimeout(&connection);
    DisarmIdleKeepAliveTimeout(&connection);
    connection.body_timeout_armed = true;
    ++connection.body_timeout_generation;

    timer_heap_.push(TimerEntry {
        CalculateBodyReceiveDeadline(now, content_length),
        fd,
        connection.connection_id,
        connection.body_timeout_generation,
        TimerKind::kBody,
    });
}

void Server::ArmIdleKeepAliveTimeout(int fd, std::chrono::steady_clock::time_point now) {
    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    ClientConnection& connection = it->second;
    DisarmHeaderTimeout(&connection);
    connection.idle_timeout_armed = true;
    ++connection.idle_timeout_generation;
    timer_heap_.push(TimerEntry {
        now + kIdleKeepAliveTimeout,
        fd,
        connection.connection_id,
        connection.idle_timeout_generation,
        TimerKind::kIdleKeepAlive,
    });
}

void Server::DisarmHeaderTimeout(ClientConnection* connection) {
    if (connection == nullptr) {
        throw std::invalid_argument("connection must not be null");
    }

    if (!connection->header_timeout_armed) {
        return;
    }

    connection->header_timeout_armed = false;
    ++connection->header_timeout_generation;
}

void Server::DisarmBodyTimeout(ClientConnection* connection) {
    if (connection == nullptr) {
        throw std::invalid_argument("connection must not be null");
    }

    if (!connection->body_timeout_armed) {
        return;
    }

    connection->body_timeout_armed = false;
    ++connection->body_timeout_generation;
}

void Server::DisarmIdleKeepAliveTimeout(ClientConnection* connection) {
    if (connection == nullptr) {
        throw std::invalid_argument("connection must not be null");
    }

    if (!connection->idle_timeout_armed) {
        return;
    }

    connection->idle_timeout_armed = false;
    ++connection->idle_timeout_generation;
}

bool Server::ShouldKeepAlive(const http::HttpRequest& request) const {
    const std::string* connection_header = request.FindHeader("connection");
    const bool wants_close =
        connection_header != nullptr &&
        ContainsHeaderToken(*connection_header, "close");
    const bool wants_keep_alive =
        connection_header != nullptr &&
        ContainsHeaderToken(*connection_header, "keep-alive");

    if (request.version() == "HTTP/1.1") {
        return !wants_close;
    }

    return wants_keep_alive;
}

// 当 inbuf/outbuf 消费、缩小或丢弃数据时，要把对应字节数同步从全局计数里扣掉。
// 这里做了保护性夹断，避免调试阶段因为重复释放导致 size_t 下溢。
void Server::ReleaseInflightBytes(std::size_t release_bytes) noexcept {
    if (release_bytes >= global_inflight_bytes_) {
        global_inflight_bytes_ = 0;
        metrics_global_inflight_bytes_current_.store(0, std::memory_order_relaxed);
        return;
    }

    global_inflight_bytes_ -= release_bytes;
    metrics_global_inflight_bytes_current_.store(global_inflight_bytes_, std::memory_order_relaxed);
}

// 带预算检查地向输入缓冲区追加数据。
// 只有 append 成功之后，global_inflight_bytes 才会真的增加。
bool Server::AppendToInputBuffer(int fd,
                                 ClientConnection* connection,
                                 const char* data,
                                 std::size_t size) {
    if (connection == nullptr) {
        throw std::invalid_argument("connection must not be null");
    }
    if (size == 0) {
        return true;
    }

    if (!TryReserveInflightBytes(fd, size, "global inflight budget exceeded while growing input buffer")) {
        return false;
    }

    connection->input_buffer.Append(data, size);
    global_inflight_bytes_ += size;
    metrics_global_inflight_bytes_current_.store(global_inflight_bytes_, std::memory_order_relaxed);
    return true;
}

// 带预算检查地向输出缓冲区尾部追加数据。
bool Server::AppendToOutputBuffer(int fd, ClientConnection* connection, std::string_view data) {
    if (connection == nullptr) {
        throw std::invalid_argument("connection must not be null");
    }
    if (data.empty()) {
        return true;
    }

    if (!TryReserveInflightBytes(fd, data.size(), "global inflight budget exceeded while growing output buffer")) {
        return false;
    }

    connection->output_buffer.append(data);
    global_inflight_bytes_ += data.size();
    metrics_global_inflight_bytes_current_.store(global_inflight_bytes_, std::memory_order_relaxed);
    return true;
}

// 用一段新的内容替换整个输出缓冲区。
// 这个函数专门处理“同一个 outbuf 可能变大，也可能变小”的情况。
bool Server::ReplaceOutputBuffer(int fd, ClientConnection* connection, std::string_view data) {
    if (connection == nullptr) {
        throw std::invalid_argument("connection must not be null");
    }

    const std::size_t old_size = connection->output_buffer.size();
    const std::size_t new_size = data.size();

    if (new_size > old_size) {
        const std::size_t growth = new_size - old_size;
        if (!TryReserveInflightBytes(fd, growth, "global inflight budget exceeded while replacing output buffer")) {
            return false;
        }

        connection->output_buffer.assign(data.data(), data.size());
        global_inflight_bytes_ += growth;
        metrics_global_inflight_bytes_current_.store(global_inflight_bytes_, std::memory_order_relaxed);
        return true;
    }

    connection->output_buffer.assign(data.data(), data.size());
    ReleaseInflightBytes(old_size - new_size);
    return true;
}

bool Server::ConsumePendingRequestBody(int fd, EventBudget* budget) {
    if (budget == nullptr) {
        throw std::invalid_argument("budget must not be null");
    }

    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return false;
    }

    ClientConnection& connection = it->second;
    if (!connection.waiting_for_request_body) {
        return true;
    }

    const std::size_t body_bytes_available = std::min(
        connection.pending_body_bytes_remaining,
        connection.input_buffer.size());
    if (body_bytes_available > 0) {
        connection.input_buffer.ConsumePrefix(body_bytes_available);
        connection.pending_body_bytes_remaining -= body_bytes_available;
        ReleaseInflightBytes(body_bytes_available);

        std::cout
            << "[Week3 Day2] consumed "
            << body_bytes_available
            << " body byte(s) from "
            << connection.peer_endpoint
            << ", body_remaining=" << connection.pending_body_bytes_remaining
            << '\n';
    }

    if (connection.pending_body_bytes_remaining > 0) {
        return true;
    }

    http::HttpRequest finished_request = connection.pending_request;
    connection.waiting_for_request_body = false;
    connection.pending_request.Clear();
    connection.pending_body_bytes_total = 0;
    connection.pending_body_bytes_remaining = 0;
    DisarmBodyTimeout(&connection);
    metrics_requests_total_.fetch_add(1, std::memory_order_relaxed);

    StartOkResponse(fd, finished_request, budget);
    return true;
}

bool Server::BeginRequestBodyReceive(int fd,
                                     const http::HttpRequest& request,
                                     EventBudget* budget) {
    if (budget == nullptr) {
        throw std::invalid_argument("budget must not be null");
    }

    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return false;
    }

    ClientConnection& connection = it->second;
    connection.waiting_for_request_body = true;
    connection.pending_request = request;
    connection.pending_body_bytes_total = request.content_length();
    connection.pending_body_bytes_remaining = request.content_length();
    ArmBodyTimeout(fd, std::chrono::steady_clock::now(), request.content_length());

    return ConsumePendingRequestBody(fd, budget);
}

// 从客户端循环读数据到应用层缓冲区。
// 只要还有预算且还有数据可读，就持续 read；
// 遇到 EAGAIN 结束本轮，遇到预算耗尽则把连接加入 ReadyQueue 在后续轮次继续读。
void Server::ReadFromClient(int fd, EventBudget* budget) {
    if (budget == nullptr) {
        throw std::invalid_argument("budget must not be null");
    }

    char buffer[kReadChunkSize];

    while (budget->read_remaining > 0) {
        auto it = clients_.find(fd);
        if (it == clients_.end() || it->second.closing) {
            return;
        }

        ClientConnection& connection = it->second;
        const std::size_t read_attempt = std::min<std::size_t>(sizeof(buffer), budget->read_remaining);
        const ssize_t bytes_read = ::read(fd, buffer, read_attempt);
        if (bytes_read > 0) {
            budget->read_remaining -= static_cast<std::size_t>(bytes_read);
            if (!AppendToInputBuffer(fd,
                                     &connection,
                                     buffer,
                                     static_cast<std::size_t>(bytes_read))) {
                return;
            }

            std::cout
                << "[Week3 Day2] read "
                << bytes_read
                << " byte(s) from "
                << connection.peer_endpoint
                << ", buffered=" << connection.input_buffer.size()
                << ", read_budget_remaining=" << budget->read_remaining
                << ", global_inflight_bytes=" << global_inflight_bytes_
                << '\n';

            if (connection.waiting_for_request_body) {
                if (!ConsumePendingRequestBody(fd, budget)) {
                    return;
                }
            } else {
                // 这条检查必须尽早做，因为文档要求的是：
                // 只要还在寻找 header 边界，且 inbuf 已经超过 8192，就立刻 431 + close。
                if (!CheckHeaderLimit(fd)) {
                    return;
                }

                if (!ProcessBufferedHeaders(fd, budget)) {
                    return;
                }
            }

            it = clients_.find(fd);
            if (it == clients_.end()) {
                return;
            }

            if (budget->read_remaining == 0) {
                it->second.ready_for_read = true;
                EnqueueReadyConnection(fd, "read budget exhausted");
                return;
            }

            continue;
        }

        if (bytes_read == 0) {
            CloseClientConnection(fd, "peer closed connection");
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        if (errno == EINTR) {
            continue;
        }

        RecordErrorMetric();
        CloseClientConnection(fd, "read() failed");
        return;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    it->second.ready_for_read = true;
    EnqueueReadyConnection(fd, "read budget exhausted");
}

// 检查请求头是否已经超过 Day5 规定的 8192 字节上限。
// 这个检查既覆盖“边界还没出现”的情况，也覆盖“边界出现了但 header 本身超限”的情况。
bool Server::CheckHeaderLimit(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return false;
    }

    if (!it->second.input_buffer.HeaderExceedsLimit(kMaxHeaderBytes)) {
        return true;
    }

    SendErrorResponseAndClose(fd,
                              431,
                              "Request Header Fields Too Large",
                              "header exceeds 8192 bytes while searching for CRLFCRLF");
    return false;
}

// 只要输入缓冲区里已经有完整 header，就循环拿出来解析。
// 和 Day6 最大的不同点是：这里不再“有多少处理多少”，而是给每轮增加上限。
// Week3 Day2 继续在这里接最小 body 状态机：
// 1. header 合法后，如果 Content-Length 为 0，就沿用现有响应流程
// 2. 如果 Content-Length > 0，就切进“等待 body 收齐”的状态，并挂 body_timeout
// 3. body 收满前，当前连接不会继续解析下一条 header
bool Server::ProcessBufferedHeaders(int fd, EventBudget* budget) {
    if (budget == nullptr) {
        throw std::invalid_argument("budget must not be null");
    }

    std::size_t processed_requests = 0;

    while (processed_requests < kMaxRequestsPerRound) {
        auto it = clients_.find(fd);
        if (it == clients_.end() || it->second.closing) {
            return false;
        }

        ClientConnection& connection = it->second;
        if (!connection.input_buffer.HasCompleteHeader()) {
            break;
        }

        const std::size_t input_size_before_pop = connection.input_buffer.size();
        std::string header = connection.input_buffer.PopNextHeader();
        ReleaseInflightBytes(input_size_before_pop - connection.input_buffer.size());
        http::HttpRequest request;
        const http::ParseError parse_error = http_parser_.ParseRequestHeader(header, &request);

        if (parse_error == http::ParseError::kNotImplementedChunked) {
            SendErrorResponseAndClose(fd,
                                      501,
                                      "Not Implemented",
                                      "chunked transfer encoding is not supported");
            return false;
        }

        if (parse_error == http::ParseError::kLengthRequired) {
            SendErrorResponseAndClose(fd,
                                      411,
                                      "Length Required",
                                      "POST request is missing Content-Length");
            return false;
        }

        if (parse_error != http::ParseError::kNone) {
            SendErrorResponseAndClose(fd,
                                      400,
                                      "Bad Request",
                                      http::ToString(parse_error));
            return false;
        }

        // Body 大小限制在 header 解析成功后立刻检查。
        // 这里不需要真正读取 body；只要 Content-Length 已经宣布超过 8MB，就应当立即 413 + close。
        if (request.has_content_length() && request.content_length() > kMaxBodyBytes) {
            SendErrorResponseAndClose(fd,
                                      413,
                                      "Payload Too Large",
                                      "Content-Length exceeds 8MB body limit");
            return false;
        }

        LogParsedRequest(connection, request);

        if (request.has_content_length() && request.content_length() > 0) {
            if (!BeginRequestBodyReceive(fd, request, budget)) {
                return false;
            }
            ++processed_requests;
            break;
        }

        metrics_requests_total_.fetch_add(1, std::memory_order_relaxed);
        StartOkResponse(fd, request, budget);
        ++processed_requests;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return false;
    }

    ClientConnection& connection = it->second;
    if (connection.waiting_for_request_body) {
        return true;
    }

    if (connection.input_buffer.HasCompleteHeader()) {
        EnqueueReadyConnection(fd, "request processing cap reached");
        return false;
    }

    if (connection.output_buffer.empty() &&
        connection.close_after_write &&
        !connection.ready_for_read) {
        CloseClientConnection(fd, "all buffered requests handled and responses sent");
        return false;
    }

    if (connection.output_buffer.empty() &&
        !connection.close_after_write &&
        !connection.ready_for_read) {
        if (connection.input_buffer.empty()) {
            ArmIdleKeepAliveTimeout(fd, std::chrono::steady_clock::now());
        } else {
            ArmHeaderTimeout(fd, std::chrono::steady_clock::now());
        }
        UpdateClientPollMask(fd);
    }

    return true;
}

// 把一条成功解析的请求摘要打印出来。
// 这样 review 时可以直接看到：
// - 请求方法是什么
// - URI 是什么
// - HTTP 版本是什么
// - 是否带了 Content-Length
void Server::LogParsedRequest(const ClientConnection& connection, const http::HttpRequest& request) const {
    std::cout
        << "[Week3 Day2] parsed request from "
        << connection.peer_endpoint
        << ": method=" << http::ToString(request.method())
        << ", uri=" << request.uri()
        << ", version=" << request.version()
        << ", header_count=" << request.headers().size();

    if (request.has_content_length()) {
        std::cout << ", content_length=" << request.content_length();
    } else {
        std::cout << ", content_length=(absent)";
    }

    std::cout << '\n';
}

// 对已经通过 Day5 解析与校验的请求，启动最小 200 OK 响应流程。
// 处理顺序是：
// 1. 先立刻尝试写一次，尽量减少额外的 epoll 往返
// 2. 如果没写完，就把剩余部分放进 outbuf
// 3. 把连接改成监听 EPOLLOUT，等下一次可写时继续发送
void Server::StartOkResponse(int fd, const http::HttpRequest& request, EventBudget* budget) {
    if (budget == nullptr) {
        throw std::invalid_argument("budget must not be null");
    }

    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    ClientConnection& connection = it->second;
    DisarmHeaderTimeout(&connection);
    DisarmIdleKeepAliveTimeout(&connection);

    const bool keep_alive = ShouldKeepAlive(request) && !connection.close_after_write;
    connection.keep_alive_enabled = keep_alive;
    if (!keep_alive) {
        connection.close_after_write = true;
    }

    const std::string response = http::BuildSimpleOkResponse(kStaticOkBody, keep_alive);

    // 如果前面已经有响应没写完，后面的响应必须直接追加到 outbuf 末尾，
    // 否则会破坏响应顺序。
    if (!connection.output_buffer.empty()) {
        if (!AppendToOutputBuffer(fd, &connection, response)) {
            return;
        }
        UpdateClientPollMask(fd);
        return;
    }

    // 如果这轮写预算已经用完，就不要再尝试 write() 了。
    // 直接把响应放进 outbuf，等后续 EPOLLOUT 再继续发送。
    if (budget->write_remaining == 0) {
        if (!ReplaceOutputBuffer(fd, &connection, response)) {
            return;
        }
        UpdateClientPollMask(fd);
        std::cout
            << "[Week3 Day2] deferred 200 OK for "
            << connection.peer_endpoint
            << " because write budget is exhausted"
            << '\n';
        return;
    }

    std::size_t written = 0;
    try {
        const std::size_t write_attempt = std::min<std::size_t>(response.size(), budget->write_remaining);
        written = net::WriteSomeNonBlocking(fd, std::string_view(response.data(), write_attempt));
    } catch (const std::system_error&) {
        RecordErrorMetric();
        CloseClientConnection(fd, "write() failed while sending 200 OK");
        return;
    }

    budget->write_remaining -= written;

    if (written >= response.size()) {
        std::cout
            << "[Week3 Day2] sent full 200 OK to "
            << connection.peer_endpoint
            << ", method=" << http::ToString(request.method())
            << ", keep_alive=" << (keep_alive ? "true" : "false")
            << '\n';
        return;
    }

    if (!ReplaceOutputBuffer(fd,
                             &connection,
                             std::string_view(response.data() + written, response.size() - written))) {
        return;
    }
    UpdateClientPollMask(fd);

    std::cout
        << "[Week3 Day2] queued "
        << connection.output_buffer.size()
        << " response byte(s) in outbuf for "
        << connection.peer_endpoint
        << ", write_budget_remaining=" << budget->write_remaining
        << ", global_inflight_bytes=" << global_inflight_bytes_
        << '\n';
}

// 继续把输出缓冲区中的剩余字节刷到 socket。
// 只要内核还能继续接收数据，就尽量多写；一旦遇到 EAGAIN 就停下，
// 保留剩余字节，等待下一次 EPOLLOUT。
void Server::FlushClientOutput(int fd, EventBudget* budget) {
    if (budget == nullptr) {
        throw std::invalid_argument("budget must not be null");
    }

    while (true) {
        auto it = clients_.find(fd);
        if (it == clients_.end() || it->second.closing) {
            return;
        }

        ClientConnection& connection = it->second;
        if (connection.output_buffer.empty()) {
            if (connection.input_buffer.HasCompleteHeader()) {
                UpdateClientPollMask(fd);
                EnqueueReadyConnection(fd, "buffer already has complete requests after flush");
                return;
            }

            if (connection.ready_for_read) {
                UpdateClientPollMask(fd);
                EnqueueReadyConnection(fd, "continue read after flush");
                return;
            }

            if (connection.close_after_write) {
                CloseClientConnection(fd, "response fully sent");
                return;
            }

            if (connection.keep_alive_enabled) {
                ArmIdleKeepAliveTimeout(fd, std::chrono::steady_clock::now());
            }
            UpdateClientPollMask(fd);
            return;
        }

        if (budget->write_remaining == 0) {
            std::cout
                << "[Week3 Day2] paused writing to "
                << connection.peer_endpoint
                << " because write budget is exhausted"
                << '\n';
            UpdateClientPollMask(fd);
            return;
        }

        std::size_t written = 0;
        try {
            const std::size_t write_attempt =
                std::min<std::size_t>(connection.output_buffer.size(), budget->write_remaining);
            written = net::WriteSomeNonBlocking(
                fd,
                std::string_view(connection.output_buffer.data(), write_attempt));
        } catch (const std::system_error&) {
            RecordErrorMetric();
            CloseClientConnection(fd, "write() failed while flushing outbuf");
            return;
        }

        if (written == 0) {
            return;
        }

        budget->write_remaining -= written;
        connection.output_buffer.erase(0, written);
        ReleaseInflightBytes(written);
        std::cout
            << "[Week3 Day2] flushed "
            << written
            << " byte(s) from outbuf to "
            << connection.peer_endpoint
            << ", remaining=" << connection.output_buffer.size()
            << ", write_budget_remaining=" << budget->write_remaining
            << ", global_inflight_bytes=" << global_inflight_bytes_
            << '\n';
    }
}

// 根据当前连接是否存在待写数据，更新 epoll 监听掩码。
void Server::UpdateClientPollMask(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end() || it->second.closing) {
        return;
    }

    std::uint32_t events = EPOLLIN | EPOLLRDHUP;
    if (!it->second.output_buffer.empty()) {
        events |= EPOLLOUT;
    }

    poller_.Modify(fd, events);
}
// 发送一个很小的错误响应，然后立即关闭连接。
// 这里采用 best-effort：
// 能写完就写完；写不完或 EAGAIN 也直接 close，不进入 Day6 的 outbuf 流程。
void Server::SendErrorResponseAndClose(int fd,
                                       int status_code,
                                       const char* reason_phrase,
                                       const char* close_reason) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    const std::string response = http::BuildSimpleErrorResponse(
        status_code,
        reason_phrase,
        std::string(reason_phrase) + "\n");

    RecordRejectMetric(status_code);
    RecordErrorMetric();
    const bool write_ok = net::WriteBestEffort(fd, response);
    std::cout
        << "[Week3 Day2] sent error response "
        << status_code
        << ' ' << reason_phrase
        << " to " << it->second.peer_endpoint
        << ", write_complete=" << (write_ok ? "true" : "false")
        << '\n';

    CloseClientConnection(fd, close_reason);
}

// 关闭并移除一个客户端连接。
// 这是连接进入关闭流程的唯一入口。
// 这一层必须统一完成：
// 1. 置 closing=true
// 2. 从 ReadyQueue 摘除
// 3. epoll_ctl(DEL)
// 4. close(fd)
// 5. 清理 fd -> connection 映射
// 6. 入 pending_close_queue，等本轮末尾再释放对象
void Server::CloseClientConnection(int fd, const char* reason) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    if (it->second.closing) {
        return;
    }

    // active_connection_count_ 的递减放在“首次进入关闭流程”的这一刻。
    // 这样做的原因是：
    // 1. 文档要求 closing=true 成为活跃连接计数递减的唯一时机
    // 2. 后续即使对象还在 pending_close_queue 里等统一释放，它也已经不再算活跃连接
    // 3. 因为 closing=true 只能在这个函数里置位，所以不会重复 -1
    it->second.closing = true;
    if (active_connection_count_ > 0) {
        --active_connection_count_;
    }
    metrics_conns_current_.store(active_connection_count_, std::memory_order_relaxed);

    it->second.in_ready_queue = false;
    it->second.ready_for_read = false;
    ready_queue_.erase(std::remove(ready_queue_.begin(), ready_queue_.end(), fd), ready_queue_.end());

    const std::string peer_endpoint = it->second.peer_endpoint;

    std::cout
        << "[Week3 Day2] closing client "
        << peer_endpoint
        << ", fd=" << fd
        << ", reason=" << reason
        << ", active_connections=" << active_connection_count_
        << ", conn_reject_total=" << conn_reject_total_
        << '\n';

    if (it->second.socket.valid()) {
        poller_.Remove(fd);
        it->second.socket.reset();
    }

    DisarmHeaderTimeout(&it->second);
    DisarmBodyTimeout(&it->second);
    DisarmIdleKeepAliveTimeout(&it->second);
    it->second.waiting_for_request_body = false;
    it->second.pending_request.Clear();
    it->second.pending_body_bytes_total = 0;
    it->second.pending_body_bytes_remaining = 0;

    // 连接关闭时，要把这个连接仍然占着的 inbuf/outbuf 字节全部归还给全局预算。
    // 这里先释放计数，再清空缓冲区内容，避免 pending_close_queue 暂存对象时继续携带旧 buffer 大小。
    const std::size_t buffered_bytes =
        it->second.input_buffer.size() + it->second.output_buffer.size();
    ReleaseInflightBytes(buffered_bytes);
    it->second.input_buffer.Clear();
    it->second.output_buffer.clear();

    PendingCloseEntry entry;
    entry.connection = std::move(it->second);
    clients_.erase(it);
    pending_close_queue_.push_back(std::move(entry));
}

// 这个函数只是把内部保存的监听 fd 暴露出来。
int Server::listening_fd() const noexcept {
    return listen_fd_.get();
}

}  // namespace core
