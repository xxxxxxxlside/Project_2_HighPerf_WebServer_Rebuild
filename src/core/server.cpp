#include "core/server.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>
#include <stdexcept>

#include "net/socket_utils.h"

namespace core {

namespace {

bool StopRequested() {
    return net::GlobalStopFlag() != 0;
}

}  // namespace

Server::Server(std::string host, std::uint16_t port, int backlog)
    : host_(std::move(host)), port_(port), backlog_(backlog) {}

void Server::Initialize() {
    // Day1 只做监听 socket 初始化，不引入 accept、epoll、HTTP 等逻辑。
    listen_fd_ = net::CreateListenSocket(host_, port_, backlog_);
}

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

int Server::listening_fd() const noexcept {
    return listen_fd_.get();
}

}  // namespace core
