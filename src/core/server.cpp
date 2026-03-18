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

// Initialize() 是 Day1 最核心的初始化入口。
// 它内部会调用 CreateListenSocket()，完成监听 socket 的建立。
void Server::Initialize() {
    // Day1 只做监听 socket 初始化，不引入 accept、epoll、HTTP 等逻辑。
    listen_fd_ = net::CreateListenSocket(host_, port_, backlog_);
}

// Run() 的职责很简单：
// 1. 确认监听 socket 已经初始化成功。
// 2. 打印当前服务监听信息。
// 3. 保持进程存活，直到收到退出信号。
void Server::Run() const {
    if (!listen_fd_.valid()) {
        throw std::logic_error("server must be initialized before run()");
    }

    // 启动日志尽量直白，便于 review 时快速确认当前版本能力边界。
    std::cout
        << "[Week1 Day1] listening on "
        << net::DescribeEndpoint(host_, port_)
        << ", backlog=" << backlog_
        << '\n';

    // Day1 只需要保持进程存活，证明监听 socket 已经成功拉起。
    while (!StopRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// 这个函数只是把内部保存的监听 fd 暴露出来。
// 当前主要用于调试；Day1 的主流程并不依赖它。
int Server::listening_fd() const noexcept {
    return listen_fd_.get();
}

}  // namespace core
