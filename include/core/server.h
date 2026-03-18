#pragma once

#include <cstdint>
#include <string>

// [Week1 Day3] New: Server 开始依赖 epoll 封装。
#include "net/epoll_poller.h"
#include "net/socket_utils.h"

namespace core {

// Server 负责当前阶段的服务端主流程。
// 到 Week1 Day3 为止，它的职责是：
// 1. 创建监听 socket
// 2. 把监听 socket 切成非阻塞
// 3. 创建 epoll 实例
// 4. 把监听 fd 注册到 epoll
// 5. 在单线程里运行最小的 Epoll LT 事件循环
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

    // [Week1 Day3] Begin: epoll 初始化与事件循环相关私有函数。
    // 创建并初始化 epoll。
    void InitializePoller();
    // 把监听 fd 注册进 epoll，监听可读事件。
    void RegisterListenSocket();
    // 处理一轮 epoll 返回的事件。
    void RunEventLoopOnce() const;
    // 处理监听 fd 上的可读事件。
    // 对于监听 socket 来说，“可读”意味着有新连接可以 accept。
    void HandleListenEvent(std::uint32_t events) const;
    // [Week1 Day3] End

    // 循环 accept 新连接，直到当前没有更多连接可接。
    // 返回本轮成功接收的连接数量，便于日志统计。
    std::size_t DrainAcceptQueue() const;

    std::string host_;
    std::uint16_t port_;
    int backlog_;

    // 监听 fd 由 RAII 封装托管，避免异常路径泄漏。
    net::UniqueFd listen_fd_;
    // [Week1 Day3] New: epoll 封装对象，当前只管理监听 fd。
    // epoll 封装对象。当前只管理监听 fd。
    mutable net::EpollPoller poller_;
};

}  // namespace core
