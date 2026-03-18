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

// Initialize() 现在对应到 Week1 Day2。
// 它会先创建监听 socket，再把监听 socket 切换为非阻塞。
void Server::Initialize() {
    // 第一步：完成 socket / SO_REUSEADDR / bind / listen。
    listen_fd_ = net::CreateListenSocket(host_, port_, backlog_);
    // 第二步：把监听 socket 切换为非阻塞，给 Day2 的 accept 循环做准备。
    MakeListenSocketNonBlocking();
}

// Run() 的职责很简单：
// 1. 确认监听 socket 已经初始化成功。
// 2. 打印当前服务监听信息。
// 3. 循环执行非阻塞 accept。
// 4. 保持进程存活，直到收到退出信号。
void Server::Run() const {
    if (!listen_fd_.valid()) {
        throw std::logic_error("server must be initialized before run()");
    }

    // 启动日志尽量直白，便于 review 时快速确认当前版本能力边界。
    std::cout
        << "[Week1 Day2] listening on "
        << net::DescribeEndpoint(host_, port_)
        << ", backlog=" << backlog_
        << ", non-blocking=true"
        << '\n';

    // Day2 开始，主循环不再只是 sleep。
    // 每一轮都会尝试把当前积压的新连接全部 accept 完，
    // 直到 accept 返回 EAGAIN/EWOULDBLOCK。
    while (!StopRequested()) {
        const std::size_t accepted_count = DrainAcceptQueue();
        // 这里先用 sleep 做一个非常朴素的轮询循环。
        // Day3 才会把这段轮询替换成真正的 epoll 事件循环。
        if (accepted_count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// 这个函数专门负责把监听 fd 切成非阻塞。
// 如果忘了这一步，accept 在没有新连接时会把线程卡住，
// 那就不符合 Day2 “非阻塞 accept 循环”的目标了。
void Server::MakeListenSocketNonBlocking() const {
    net::SetNonBlocking(listen_fd_.get());
}

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
            << "[Week1 Day2] accepted connection from "
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
