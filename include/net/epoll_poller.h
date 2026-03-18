#pragma once

#include <cstdint>
#include <sys/epoll.h>
#include <vector>

#include "net/socket_utils.h"

namespace net {

// PollEvent 是对 epoll 返回结果的一层轻量封装。
// 当前阶段只需要关心：
// 1. 哪个 fd 就绪了
// 2. 就绪事件是什么
struct PollEvent {
    int fd = -1;
    std::uint32_t events = 0;
};

// EpollPoller 是当前项目里最小的 epoll 封装。
// 它的职责只保留三类动作：
// 1. 创建 epoll 实例
// 2. 注册 / 修改 / 删除 fd
// 3. 等待一轮事件
class EpollPoller {
public:
    // 默认构造：先不创建 epoll fd，真正创建动作放到 Open()。
    EpollPoller();
    // 析构使用默认实现即可，内部 fd 由 UniqueFd 自动释放。
    ~EpollPoller() = default;

    // 禁止拷贝，避免一个 epoll fd 被多个对象重复管理。
    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    // 创建 epoll 实例。
    void Open();
    // 注册一个 fd 及其监听事件。
    void Add(int fd, std::uint32_t events);
    // 修改已注册 fd 的监听事件。
    void Modify(int fd, std::uint32_t events);
    // 删除一个已注册 fd。
    void Remove(int fd);
    // 等待一轮事件。
    // timeout_ms 的单位是毫秒：
    // -1 表示无限等待
    //  0 表示立即返回
    std::vector<PollEvent> Wait(int timeout_ms);

    // 返回 epoll fd 是否已经成功创建。
    [[nodiscard]] bool IsOpen() const noexcept;

private:
    // 所有 epoll_ctl 操作都统一走这个函数，减少重复代码。
    void Control(int operation, int fd, std::uint32_t events);
    // 如果某一轮事件数量打满当前缓存，就扩容，避免后续频繁截断。
    void GrowReadyEventsIfNeeded(std::size_t ready_count);

    // epoll fd 本身也交给 UniqueFd 管理。
    UniqueFd epoll_fd_;
    // 保存 epoll_wait 返回结果的缓冲区。
    std::vector<struct epoll_event> ready_events_;
};

}  // namespace net
