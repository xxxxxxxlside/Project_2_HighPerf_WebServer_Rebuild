#pragma once

#include <cstdint>
#include <string>

#include "net/socket_utils.h"

namespace core {

// Server 负责 Day1 的最小服务端闭环：
// 创建监听 socket，并在运行时保持进程存活。
class Server {
public:
    Server(std::string host, std::uint16_t port, int backlog);
    ~Server() = default;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // 完成 Day1 初始化：socket + SO_REUSEADDR + bind + listen。
    void Initialize();
    // 进入运行态，直到收到退出信号。
    void Run() const;

    [[nodiscard]] int listening_fd() const noexcept;

private:
    std::string host_;
    std::uint16_t port_;
    int backlog_;

    // 监听 fd 由 RAII 封装托管，避免异常路径泄漏。
    net::UniqueFd listen_fd_;
};

}  // namespace core
