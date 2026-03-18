#include "core/server.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <stdexcept>
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

// Day6 先不引入真正的路由系统，先用一个静态文本把正常响应链路走通。
constexpr std::string_view kStaticOkBody = "Week2 Day2 static response\n";

// Week2 Day1 要求单连接单轮最多只处理 5 个完整请求。
// 这样做的目的是避免一个连接持续占用 EventLoop，导致其它连接饥饿。
constexpr std::size_t kMaxRequestsPerRound = 5;

// Week2 Day2 新增单轮读/写预算。
// 这两个预算都按“单连接、单轮处理”计算，而不是全局共享。
constexpr std::size_t kMaxReadBytesPerEvent = 256 * 1024;
constexpr std::size_t kMaxWriteBytesPerEvent = 256 * 1024;

}  // namespace

// 构造函数只保存配置，不做系统调用。
Server::Server(std::string host, std::uint16_t port, int backlog)
    : host_(std::move(host)), port_(port), backlog_(backlog) {}

// Initialize() 负责完成服务启动前的准备工作。
// 到 Week2 Day2 为止，这里仍然只做监听相关和事件循环相关的初始化。
void Server::Initialize() {
    listen_fd_ = net::CreateListenSocket(host_, port_, backlog_);
    MakeListenSocketNonBlocking();
    InitializePoller();
    RegisterListenSocket();
}

// Run() 会一直阻塞在事件循环里，直到外部发来退出信号。
void Server::Run() {
    if (!listen_fd_.valid() || !poller_.IsOpen()) {
        throw std::logic_error("server must be initialized before run()");
    }

    std::cout
        << "[Week2 Day2] listening on "
        << net::DescribeEndpoint(host_, port_)
        << ", backlog=" << backlog_
        << ", non-blocking=true"
        << ", epoll=LT"
        << '\n';

    while (!StopRequested()) {
        RunEventLoopOnce();
    }
}

// 把监听 fd 切成非阻塞。
void Server::MakeListenSocketNonBlocking() const {
    net::SetNonBlocking(listen_fd_.get());
}

// 创建 epoll 实例。
void Server::InitializePoller() {
    poller_.Open();
}

// 把监听 fd 注册进 epoll。
void Server::RegisterListenSocket() {
    poller_.Add(listen_fd_.get(), EPOLLIN);
}

// 跑一轮 epoll_wait，然后把返回的事件逐个分发出去。
void Server::RunEventLoopOnce() {
    ProcessReadyQueue();

    // 如果 ReadyQueue 里还有待处理连接，就不要在 epoll_wait 上长时间阻塞，
    // 这样下一轮可以尽快继续把这些“用户态 buffer 中已就绪”的请求处理掉。
    const int wait_timeout_ms = ready_queue_.empty() ? 1000 : 0;
    const std::vector<net::PollEvent> events = poller_.Wait(wait_timeout_ms);

    for (const net::PollEvent& event : events) {
        HandlePollEvent(event);
    }
}

// [Week2 Day2] Begin: ReadyQueue 现在同时承接“请求处理上限”和“读预算耗尽”两种续处理场景。

// 处理一轮 ReadyQueue。
// 这里故意只处理“本轮开始时队列中已有的元素”，不把新入队的连接继续在同一轮吃完，
// 这样可以把“下一轮再处理”的边界保持清晰，避免一个连接反复自我入队后独占 EventLoop。
void Server::ProcessReadyQueue() {
    const std::size_t ready_count = ready_queue_.size();
    for (std::size_t i = 0; i < ready_count; ++i) {
        const int fd = ready_queue_.front();
        ready_queue_.pop_front();

        auto it = clients_.find(fd);
        if (it == clients_.end()) {
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
// 2. 再 parse：如果 input_buffer 里已经有完整请求，优先消费这些“已经在用户态可见”的工作
// 3. 最后读：如果上轮是因为读预算耗尽才停下，就继续 read
void Server::ContinueReadyConnection(int fd, EventBudget* budget) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    if (!it->second.output_buffer.empty()) {
        FlushClientOutput(fd, budget);
    }

    it = clients_.find(fd);
    if (it == clients_.end() || !it->second.output_buffer.empty()) {
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
    if (it == clients_.end()) {
        return;
    }

    if (it->second.in_ready_queue) {
        return;
    }

    it->second.in_ready_queue = true;
    ready_queue_.push_back(fd);

    std::cout
        << "[Week2 Day2] queued client "
        << it->second.peer_endpoint
        << " into ReadyQueue, reason=" << reason
        << ", buffered=" << it->second.input_buffer.size()
        << '\n';
}

// [Week2 Day2] End

// 统一分发一轮事件。
void Server::HandlePollEvent(const net::PollEvent& event) {
    if (event.fd == listen_fd_.get()) {
        HandleListenEvent(event.events);
        return;
    }

    if (clients_.find(event.fd) != clients_.end()) {
        HandleClientEvent(event.fd, event.events);
    }
}

// 处理监听 fd 的事件。
void Server::HandleListenEvent(std::uint32_t events) {
    if ((events & EPOLLIN) == 0U) {
        return;
    }

    const std::size_t accepted_count = DrainAcceptQueue();
    if (accepted_count > 0) {
        std::cout
            << "[Week2 Day2] drained "
            << accepted_count
            << " connection(s) from listen queue"
            << '\n';
    }
}

// 处理客户端 fd 的事件。
// 到 Week2 Day2 为止，连接可能同时出现：
// 1. 读事件：继续接收请求头
// 2. 写事件：把 outbuf 里未发完的响应继续写出去
// 3. 关闭/错误事件：直接回收连接
void Server::HandleClientEvent(int fd, std::uint32_t events) {
    if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
        CloseClientConnection(fd, "peer hangup or error event");
        return;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end()) {
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
        RegisterAcceptedConnection(std::move(accepted));
    }

    return accepted_count;
}

// 接到新连接后，把它纳入最小连接表，并注册到 epoll。
void Server::RegisterAcceptedConnection(net::AcceptedSocket accepted) {
    const int client_fd = accepted.fd.get();
    if (client_fd < 0) {
        throw std::logic_error("accepted socket must be valid");
    }

    ClientConnection connection;
    connection.socket = std::move(accepted.fd);
    connection.peer_endpoint = std::move(accepted.peer_endpoint);

    poller_.Add(client_fd, EPOLLIN | EPOLLRDHUP);
    clients_.emplace(client_fd, std::move(connection));

    std::cout
        << "[Week2 Day2] registered client "
        << clients_.at(client_fd).peer_endpoint
        << ", fd=" << client_fd
        << '\n';
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
        if (it == clients_.end()) {
            return;
        }

        ClientConnection& connection = it->second;
        const std::size_t read_attempt = std::min<std::size_t>(sizeof(buffer), budget->read_remaining);
        const ssize_t bytes_read = ::read(fd, buffer, read_attempt);
        if (bytes_read > 0) {
            budget->read_remaining -= static_cast<std::size_t>(bytes_read);
            connection.input_buffer.Append(buffer, static_cast<std::size_t>(bytes_read));

            std::cout
                << "[Week2 Day2] read "
                << bytes_read
                << " byte(s) from "
                << connection.peer_endpoint
                << ", buffered=" << connection.input_buffer.size()
                << ", read_budget_remaining=" << budget->read_remaining
                << '\n';

            // 这条检查必须尽早做，因为文档要求的是：
            // 只要还在寻找 header 边界，且 inbuf 已经超过 8192，就立刻 431 + close。
            if (!CheckHeaderLimit(fd)) {
                return;
            }

            if (!ProcessBufferedHeaders(fd, budget)) {
                return;
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

        CloseClientConnection(fd, "read() failed");
        return;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end()) {
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

// [Week2 Day2] Begin: 在保留“5 个完整请求上限”的同时，引入单轮读预算控制。

// 只要输入缓冲区里已经有完整 header，就循环拿出来解析。
// 和 Day6 最大的不同点是：这里不再“有多少处理多少”，而是给每轮增加上限。
// 一旦达到上限但 buffer 里还有完整请求，就把连接丢进 ReadyQueue，留到后续轮次继续处理。
bool Server::ProcessBufferedHeaders(int fd, EventBudget* budget) {
    if (budget == nullptr) {
        throw std::invalid_argument("budget must not be null");
    }

    std::size_t processed_requests = 0;

    while (processed_requests < kMaxRequestsPerRound) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return false;
        }

        ClientConnection& connection = it->second;
        if (!connection.input_buffer.HasCompleteHeader()) {
            break;
        }

        std::string header = connection.input_buffer.PopNextHeader();
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

        LogParsedRequest(connection, request);

        // 当前阶段仍然只基于 header 做最小决策，不进入后续的 body 状态机。
        StartOkResponse(fd, request, budget);
        ++processed_requests;
    }

    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return false;
    }

    ClientConnection& connection = it->second;
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

    return true;
}

// [Week2 Day2] End

// 把一条成功解析的请求摘要打印出来。
// 这样 review 时可以直接看到：
// - 请求方法是什么
// - URI 是什么
// - HTTP 版本是什么
// - 是否带了 Content-Length
void Server::LogParsedRequest(const ClientConnection& connection, const http::HttpRequest& request) const {
    std::cout
        << "[Week2 Day2] parsed request from "
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

// [Week2 Day2] Begin: 响应写回链路增加单轮写预算，避免大 outbuf 长时间独占 EventLoop。

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
    if (it == clients_.end()) {
        return;
    }

    ClientConnection& connection = it->second;
    const std::string response = http::BuildSimpleOkResponse(kStaticOkBody);
    connection.close_after_write = true;

    // 如果前面已经有响应没写完，后面的响应必须直接追加到 outbuf 末尾，
    // 否则会破坏响应顺序。
    if (!connection.output_buffer.empty()) {
        connection.output_buffer.append(response);
        UpdateClientPollMask(fd);
        return;
    }

    // 如果这轮写预算已经用完，就不要再尝试 write() 了。
    // 直接把响应放进 outbuf，等后续 EPOLLOUT 再继续发送。
    if (budget->write_remaining == 0) {
        connection.output_buffer = response;
        UpdateClientPollMask(fd);
        std::cout
            << "[Week2 Day2] deferred 200 OK for "
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
        CloseClientConnection(fd, "write() failed while sending 200 OK");
        return;
    }

    budget->write_remaining -= written;

    if (written >= response.size()) {
        std::cout
            << "[Week2 Day2] sent full 200 OK to "
            << connection.peer_endpoint
            << ", method=" << http::ToString(request.method())
            << '\n';
        return;
    }

    connection.output_buffer.assign(response.data() + written, response.size() - written);
    UpdateClientPollMask(fd);

    std::cout
        << "[Week2 Day2] queued "
        << connection.output_buffer.size()
        << " response byte(s) in outbuf for "
        << connection.peer_endpoint
        << ", write_budget_remaining=" << budget->write_remaining
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
        if (it == clients_.end()) {
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

            UpdateClientPollMask(fd);
            return;
        }

        if (budget->write_remaining == 0) {
            std::cout
                << "[Week2 Day2] paused writing to "
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
            CloseClientConnection(fd, "write() failed while flushing outbuf");
            return;
        }

        if (written == 0) {
            return;
        }

        budget->write_remaining -= written;
        connection.output_buffer.erase(0, written);
        std::cout
            << "[Week2 Day2] flushed "
            << written
            << " byte(s) from outbuf to "
            << connection.peer_endpoint
            << ", remaining=" << connection.output_buffer.size()
            << ", write_budget_remaining=" << budget->write_remaining
            << '\n';
    }
}

// 根据当前连接是否存在待写数据，更新 epoll 监听掩码。
void Server::UpdateClientPollMask(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    std::uint32_t events = EPOLLIN | EPOLLRDHUP;
    if (!it->second.output_buffer.empty()) {
        events |= EPOLLOUT;
    }

    poller_.Modify(fd, events);
}

// [Week2 Day2] End

// [Week2 Day2] Begin: 统一收口当前阶段的错误响应与关闭流程。

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

    const bool write_ok = net::WriteBestEffort(fd, response);
    std::cout
        << "[Week2 Day2] sent error response "
        << status_code
        << ' ' << reason_phrase
        << " to " << it->second.peer_endpoint
        << ", write_complete=" << (write_ok ? "true" : "false")
        << '\n';

    CloseClientConnection(fd, close_reason);
}

// [Week2 Day2] End

// 关闭并移除一个客户端连接。
// 当前阶段还没有 Week2 后续要做的 pending_close_queue，所以这里直接收掉即可。
// 但为了避免 ReadyQueue 里留下旧 fd，这里会把对应标记清掉。
void Server::CloseClientConnection(int fd, const char* reason) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    it->second.in_ready_queue = false;
    it->second.ready_for_read = false;
    ready_queue_.erase(std::remove(ready_queue_.begin(), ready_queue_.end(), fd), ready_queue_.end());

    std::cout
        << "[Week2 Day2] closing client "
        << it->second.peer_endpoint
        << ", fd=" << fd
        << ", reason=" << reason
        << '\n';

    poller_.Remove(fd);
    clients_.erase(it);
}

// 这个函数只是把内部保存的监听 fd 暴露出来。
int Server::listening_fd() const noexcept {
    return listen_fd_.get();
}

}  // namespace core
