#pragma once

#include <cstdint>
#include <string>

#include "net/socket_utils.h"

namespace core {

// Server 负责 Day1 的最小服务端闭环：
// 创建监听 socket，并在运行时保持进程存活。
class Server {
public:
    // 构造函数：
    // 把监听地址、端口和 backlog 保存下来，
    // 真正创建 socket 的动作放到 Initialize() 里做。
    Server(std::string host, std::uint16_t port, int backlog);
    // 析构函数使用默认实现即可；
    // 因为 listen_fd_ 是 RAII 对象，会自动 close。
    ~Server() = default;

    // 禁止拷贝，避免同一个监听 fd 被多个对象误管理。
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // 完成 Day1 初始化：socket + SO_REUSEADDR + bind + listen。
    void Initialize();
    // 进入运行态，直到收到退出信号。
    void Run() const;

    // 返回当前监听 fd，主要用于调试和排错。
    [[nodiscard]] int listening_fd() const noexcept;

private:
    // 把监听 socket 设置为非阻塞。
    // 这是 Week1 Day2 的关键步骤之一。
    void MakeListenSocketNonBlocking() const;

    // 循环 accept 新连接，直到当前没有更多连接可接。
    // 返回本轮成功接收的连接数量，便于日志统计。
    std::size_t DrainAcceptQueue() const;

    std::string host_;
    std::uint16_t port_;
    int backlog_;

    // 监听 fd 由 RAII 封装托管，避免异常路径泄漏。
    net::UniqueFd listen_fd_;
};

}  // namespace core
