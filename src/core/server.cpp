#include "core/server.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>

#include "net/socket_utils.h"

namespace core {

namespace {

// 这个辅助函数专门读取全局退出标记。
// Server::Run() 会轮询它，决定是否结束事件循环。
bool StopRequested() {
    return net::GlobalStopFlag() != 0;
}

// Day4 当前每次 read() 读一个固定大小的小块。
// 这里用 4KB 只是为了演示“多次读取 -> 追加到动态缓冲区 -> 再按应用层边界切分”的流程。
constexpr std::size_t kReadChunkSize = 4096;

}  // namespace

// 构造函数只保存配置，不做系统调用。
// 这样 main.cpp 可以先创建对象，再明确调用 Initialize()。
Server::Server(std::string host, std::uint16_t port, int backlog)
    : host_(std::move(host)), port_(port), backlog_(backlog) {}

// Initialize() 负责完成服务启动前的准备工作。
// 到 Day4 为止，这里仍然只做监听相关和事件循环相关的初始化，
// 不做任何 HTTP 解析和响应写回。
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
        << "[Week1 Day4] listening on "
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
// 这样没有新连接时，accept 不会把线程卡住。
void Server::MakeListenSocketNonBlocking() const {
    net::SetNonBlocking(listen_fd_.get());
}

// 创建 epoll 实例。
void Server::InitializePoller() {
    poller_.Open();
}

// 把监听 fd 注册进 epoll。
// 这里只监听 EPOLLIN，配合 LT 模式即可。
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
// 这样做的好处是：
// 1. 监听 fd 和客户端 fd 的处理路径分开，代码更清楚
// 2. 初学者 review 时更容易看出“不同 fd 的职责不同”
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
// 对监听 socket 来说，EPOLLIN 表示“有新连接到达，可以继续 accept 了”。
void Server::HandleListenEvent(std::uint32_t events) {
    if ((events & EPOLLIN) == 0U) {
        return;
    }

    const std::size_t accepted_count = DrainAcceptQueue();
    if (accepted_count > 0) {
        std::cout
            << "[Week1 Day4] drained "
            << accepted_count
            << " connection(s) from listen queue"
            << '\n';
    }
}

// 处理客户端 fd 的事件。
// Day4 当前只关心“可读事件”，因为本阶段目标是把数据读进应用层缓冲区并切出完整请求头。
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
// 这仍然沿用 Day2 的“accept until EAGAIN”策略，只是触发时机改成了 epoll 事件。
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

// [Week1 Day4] Begin: 把新连接纳入最小连接表，并为它准备应用层输入缓冲区。

// 接到新连接后，不再像 Day3 那样立刻关闭。
// Day4 需要把连接保留下来，因为后面要在这个连接上接收数据并处理 TCP 粘包/拆包。
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
        << "[Week1 Day4] registered client "
        << clients_.at(client_fd).peer_endpoint
        << ", fd=" << client_fd
        << '\n';
}

// [Week1 Day4] End

// [Week1 Day4] Begin: 从 socket 读数据到动态缓冲区，并按 "\r\n\r\n" 切完整请求头。

// 这是 Day4 的核心函数之一。
// 它会不断 read()，直到：
// 1. 暂时没有更多数据可读（EAGAIN / EWOULDBLOCK）
// 2. 对端主动关闭连接
// 3. 出现真正的读错误
void Server::ReadFromClient(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    ClientConnection& connection = it->second;
    char buffer[kReadChunkSize];

    while (true) {
        const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            // 先把这一小块字节追加进应用层缓冲区。
            // 这样即使请求头被拆成多次 read() 才收全，也能重新拼起来。
            connection.input_buffer.Append(buffer, static_cast<std::size_t>(bytes_read));

            std::cout
                << "[Week1 Day4] read "
                << bytes_read
                << " byte(s) from "
                << connection.peer_endpoint
                << ", buffered=" << connection.input_buffer.size()
                << '\n';

            // 每次追加完数据后，都试着看看缓冲区里有没有完整请求头。
            ProcessBufferedHeaders(connection);
            continue;
        }

        if (bytes_read == 0) {
            // read() 返回 0 表示对端已经正常关闭连接。
            CloseClientConnection(fd, "peer closed connection");
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 非阻塞读到这里，说明“当前这一轮已经读空了”。
            // LT 模式下这正是我们本轮读循环应该结束的时机。
            return;
        }

        if (errno == EINTR) {
            // 被信号打断时，直接重试这次 read。
            continue;
        }

        CloseClientConnection(fd, "read() failed");
        return;
    }
}

// 处理一个连接缓冲区里已经完整的请求头。
// Day4 当前只做边界切分，不做 Day5 的请求行解析和 Header 语义解析。
void Server::ProcessBufferedHeaders(ClientConnection& connection) {
    while (connection.input_buffer.HasCompleteHeader()) {
        std::string header = connection.input_buffer.PopNextHeader();

        std::cout
            << "[Week1 Day4] extracted complete header from "
            << connection.peer_endpoint
            << ", header_bytes=" << header.size()
            << '\n';

        // 为了让 review 时更直观看到“切出来的边界到底长什么样”，
        // 这里把完整请求头原样打印出来。
        std::cout
            << "[Week1 Day4] header content begin\n"
            << header
            << "[Week1 Day4] header content end\n";
    }
}

// [Week1 Day4] End

// 关闭并移除一个客户端连接。
// 这里的顺序是：
// 1. 先从 epoll 删除
// 2. 再从连接表删除
// 3. 删除时 UniqueFd 自动 close
//
// 当前阶段还没有 Week2 的 pending_close_queue，所以这里直接收掉即可。
void Server::CloseClientConnection(int fd, const char* reason) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    std::cout
        << "[Week1 Day4] closing client "
        << it->second.peer_endpoint
        << ", fd=" << fd
        << ", reason=" << reason
        << '\n';

    poller_.Remove(fd);
    clients_.erase(it);
}

// 这个函数只是把内部保存的监听 fd 暴露出来。
// 当前主要用于调试；主流程并不依赖它。
int Server::listening_fd() const noexcept {
    return listen_fd_.get();
}

}  // namespace core
