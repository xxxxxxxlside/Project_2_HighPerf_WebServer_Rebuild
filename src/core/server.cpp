#include "core/server.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
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

}  // namespace

// 构造函数只保存配置，不做系统调用。
Server::Server(std::string host, std::uint16_t port, int backlog)
    : host_(std::move(host)), port_(port), backlog_(backlog) {}

// Initialize() 负责完成服务启动前的准备工作。
// 到 Day5 为止，这里仍然只做监听相关和事件循环相关的初始化。
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
        << "[Week1 Day5] listening on "
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
    constexpr int kWaitTimeoutMs = 1000;
    const std::vector<net::PollEvent> events = poller_.Wait(kWaitTimeoutMs);

    for (const net::PollEvent& event : events) {
        HandlePollEvent(event);
    }
}

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
            << "[Week1 Day5] drained "
            << accepted_count
            << " connection(s) from listen queue"
            << '\n';
    }
}

// 处理客户端 fd 的事件。
// 到 Day5 为止，这里依然只关心“读事件”和“对端关闭/错误事件”。
void Server::HandleClientEvent(int fd, std::uint32_t events) {
    if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
        CloseClientConnection(fd, "peer hangup or error event");
        return;
    }

    if ((events & EPOLLIN) != 0U) {
        ReadFromClient(fd);
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
        << "[Week1 Day5] registered client "
        << clients_.at(client_fd).peer_endpoint
        << ", fd=" << client_fd
        << '\n';
}

// 从客户端循环读数据到应用层缓冲区。
// 只要还有数据可读，就持续 read；直到遇到 EAGAIN 才结束这一轮。
void Server::ReadFromClient(int fd) {
    char buffer[kReadChunkSize];

    while (true) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return;
        }

        ClientConnection& connection = it->second;
        const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            connection.input_buffer.Append(buffer, static_cast<std::size_t>(bytes_read));

            std::cout
                << "[Week1 Day5] read "
                << bytes_read
                << " byte(s) from "
                << connection.peer_endpoint
                << ", buffered=" << connection.input_buffer.size()
                << '\n';

            // [Week1 Day5] Begin: 在“还没找到 \r\n\r\n”时，立即执行 8192 header 上限防御。
            // 这条检查必须尽早做，因为文档要求的是：
            // 只要还在寻找 header 边界，且 inbuf 已经超过 8192，就立刻 431 + close。
            if (!CheckHeaderLimit(fd)) {
                return;
            }
            // [Week1 Day5] End

            ProcessBufferedHeaders(fd);
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
}

// [Week1 Day5] Begin: 对完整请求头做 HTTP 解析，并执行 Day5 的协议边界防御。

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
// 这样可以处理“一次 read() 收到多个请求头”的粘包情况。
void Server::ProcessBufferedHeaders(int fd) {
    while (true) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return;
        }

        ClientConnection& connection = it->second;
        if (!connection.input_buffer.HasCompleteHeader()) {
            return;
        }

        std::string header = connection.input_buffer.PopNextHeader();
        http::HttpRequest request;
        const http::ParseError parse_error = http_parser_.ParseRequestHeader(header, &request);

        if (parse_error == http::ParseError::kNotImplementedChunked) {
            SendErrorResponseAndClose(fd,
                                      501,
                                      "Not Implemented",
                                      "chunked transfer encoding is not supported");
            return;
        }

        if (parse_error == http::ParseError::kLengthRequired) {
            SendErrorResponseAndClose(fd,
                                      411,
                                      "Length Required",
                                      "POST request is missing Content-Length");
            return;
        }

        if (parse_error != http::ParseError::kNone) {
            SendErrorResponseAndClose(fd,
                                      400,
                                      "Bad Request",
                                      http::ToString(parse_error));
            return;
        }

        LogParsedRequest(connection, request);

        // Day5 当前只负责“把头解析明白”和“做协议防御”。
        // Day6 之前还没有正常的 200 OK 写回和 body 处理流程，
        // 所以这里在成功解析后直接关闭连接，避免把后续未实现的状态机混进来。
        CloseClientConnection(fd, "header parsed successfully; response path starts in Day6");
        return;
    }
}

// [Week1 Day5] End

// 把一条成功解析的请求摘要打印出来。
// 这样 review 时可以直接看到：
// - 请求方法是什么
// - URI 是什么
// - HTTP 版本是什么
// - 是否带了 Content-Length
void Server::LogParsedRequest(const ClientConnection& connection, const http::HttpRequest& request) const {
    std::cout
        << "[Week1 Day5] parsed request from "
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

// [Week1 Day5] Begin: 统一收口 Day5 错误响应与关闭流程。

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
        << "[Week1 Day5] sent error response "
        << status_code
        << ' ' << reason_phrase
        << " to " << it->second.peer_endpoint
        << ", write_complete=" << (write_ok ? "true" : "false")
        << '\n';

    CloseClientConnection(fd, close_reason);
}

// [Week1 Day5] End

// 关闭并移除一个客户端连接。
// 当前阶段还没有 Week2 的 pending_close_queue，所以这里直接收掉即可。
void Server::CloseClientConnection(int fd, const char* reason) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    std::cout
        << "[Week1 Day5] closing client "
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
