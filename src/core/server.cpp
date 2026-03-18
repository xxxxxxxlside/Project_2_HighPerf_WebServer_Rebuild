#include "core/server.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>
#include <stdexcept>

#include "net/socket_utils.h"

namespace core {

namespace {

// 这个辅助函数专门读取全局退出标记。
// Server::Run() 会轮询它，决定是否结束最小运行循环。
bool StopRequested() {
    return net::GlobalStopFlag() != 0;
}

}  // namespace

// 构造函数只保存配置，不做系统调用。
// 这样 main.cpp 可以先创建对象，再明确调用 Initialize()。
Server::Server(std::string host, std::uint16_t port, int backlog)
    : host_(std::move(host)), port_(port), backlog_(backlog) {}

// Initialize() 现在对应到 Week1 Day3。
// 它会依次完成：
// 1. 创建监听 socket
// 2. 把监听 socket 切成非阻塞
// 3. 创建 epoll
// 4. 把监听 fd 注册到 epoll
void Server::Initialize() {
    // 第一步：完成 socket / SO_REUSEADDR / bind / listen。
    listen_fd_ = net::CreateListenSocket(host_, port_, backlog_);
    // 第二步：把监听 socket 切换为非阻塞，给 Day2 的 accept 循环做准备。
    MakeListenSocketNonBlocking();

    // [Week1 Day3] Begin: 把 Day2 的监听 socket 接入 epoll。
    // 第三步：创建 epoll。
    InitializePoller();
    // 第四步：把监听 fd 加入 epoll。
    RegisterListenSocket();
    // [Week1 Day3] End
}

// Run() 的职责很简单：
// 1. 确认监听 socket 已经初始化成功。
// 2. 打印当前服务监听信息。
// 3. 进入单线程 Epoll LT 事件循环。
void Server::Run() const {
    if (!listen_fd_.valid() || !poller_.IsOpen()) {
        throw std::logic_error("server must be initialized before run()");
    }

    // 启动日志尽量直白，便于 review 时快速确认当前版本能力边界。
    std::cout
        << "[Week1 Day3] listening on "
        << net::DescribeEndpoint(host_, port_)
        << ", backlog=" << backlog_
        << ", non-blocking=true"
        << ", epoll=LT"
        << '\n';

    // [Week1 Day3] Begin: 主循环从“轮询 + sleep”切到 epoll_wait 驱动。
    // 从 Day3 开始，不再主动 sleep 轮询新连接，
    // 而是交给 epoll_wait 在事件到来时唤醒当前线程。
    while (!StopRequested()) {
        RunEventLoopOnce();
    }
    // [Week1 Day3] End
}

// 这个函数专门负责把监听 fd 切成非阻塞。
// 如果忘了这一步，accept 在没有新连接时会把线程卡住，
// 那就不符合 Day2 “非阻塞 accept 循环”的目标了。
void Server::MakeListenSocketNonBlocking() const {
    net::SetNonBlocking(listen_fd_.get());
}

// [Week1 Day3] Begin: epoll 初始化与事件分发函数。

// 创建 epoll 实例。
// 这是 Day3 从“手动轮询”切到“事件驱动”的第一步。
void Server::InitializePoller() {
    poller_.Open();
}

// 把监听 fd 注册到 epoll。
// 这里用的只是 EPOLLIN，没有加 EPOLLET，
// 所以这就是文档要求的 LT 模式。
void Server::RegisterListenSocket() {
    poller_.Add(listen_fd_.get(), EPOLLIN);
}

// 运行一轮 epoll 事件循环。
// 当前版本只关心监听 fd 的可读事件。
void Server::RunEventLoopOnce() const {
    constexpr int kWaitTimeoutMs = 1000;
    const std::vector<net::PollEvent> events = poller_.Wait(kWaitTimeoutMs);

    for (const net::PollEvent& event : events) {
        if (event.fd == listen_fd_.get()) {
            HandleListenEvent(event.events);
        }
    }
}

// 处理监听 fd 的事件。
// 对监听 socket 来说，EPOLLIN 表示“有新连接到达，可以继续 accept 了”。
void Server::HandleListenEvent(std::uint32_t events) const {
    if ((events & EPOLLIN) != 0U) {
        const std::size_t accepted_count = DrainAcceptQueue();
        if (accepted_count > 0) {
            std::cout
                << "[Week1 Day3] drained "
                << accepted_count
                << " connection(s) from listen queue via epoll LT"
                << '\n';
        }
    }
}

// [Week1 Day3] End

// 不断调用 TryAcceptOne()，把内核里当前已经排队的新连接尽量一次接完。
// 这就是文档里说的“accept loop”。
//
// 当前阶段我们只做到：
// - accept 成功
// - 记录日志
// - 让连接 fd 以 RAII 方式在本轮结束后自动 close
//
// 之所以先 close，是因为 Day2 还没有开始做连接管理、读写和协议解析。
std::size_t Server::DrainAcceptQueue() const {
    std::size_t accepted_count = 0;

    while (true) {
        net::AcceptedSocket accepted;
        if (!net::TryAcceptOne(listen_fd_.get(), &accepted)) {
            break;
        }

        ++accepted_count;
        std::cout
            << "[Week1 Day3] accepted connection from "
            << accepted.peer_endpoint
            << ", fd=" << accepted.fd.get()
            << " (current stage closes it immediately after accept)"
            << '\n';
    }

    return accepted_count;
}

// 这个函数只是把内部保存的监听 fd 暴露出来。
// 当前主要用于调试；Day1 的主流程并不依赖它。
int Server::listening_fd() const noexcept {
    return listen_fd_.get();
}

}  // namespace core
