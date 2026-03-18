#include "net/epoll_poller.h"

#include <cerrno>
#include <stdexcept>
#include <sys/epoll.h>
#include <system_error>

namespace {

// 把 epoll 相关 errno 统一包装成异常，便于上层复用一套错误处理路径。
void ThrowLastSystemError(const char* message) {
    throw std::system_error(errno, std::generic_category(), message);
}

}  // namespace

namespace net {

// 构造函数先给事件缓冲区一个较小的初始容量。
// 当前连接数还不大，这个默认值足够；如果事件打满，会在运行时自动扩容。
EpollPoller::EpollPoller() : ready_events_(16) {}

// 创建 epoll fd。
// 这里使用 EPOLL_CLOEXEC，避免 exec 后子进程继承这个 fd。
void EpollPoller::Open() {
    if (epoll_fd_.valid()) {
        return;
    }

    epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_fd_.valid()) {
        ThrowLastSystemError("epoll_create1() failed");
    }
}

// 注册一个新的 fd 到 epoll。
void EpollPoller::Add(int fd, std::uint32_t events) {
    Control(EPOLL_CTL_ADD, fd, events);
}

// 修改一个已经注册过的 fd 的事件掩码。
void EpollPoller::Modify(int fd, std::uint32_t events) {
    Control(EPOLL_CTL_MOD, fd, events);
}

// 从 epoll 中删除一个 fd。
void EpollPoller::Remove(int fd) {
    Control(EPOLL_CTL_DEL, fd, 0);
}

// 等待一轮 epoll 事件。
// 当前项目采用 LT 模式，所以这里只要注册 EPOLLIN 而不加 EPOLLET 即可。
std::vector<PollEvent> EpollPoller::Wait(int timeout_ms) {
    if (!epoll_fd_.valid()) {
        throw std::logic_error("epoll poller is not open");
    }

    const int ready_count = ::epoll_wait(epoll_fd_.get(),
                                         ready_events_.data(),
                                         static_cast<int>(ready_events_.size()),
                                         timeout_ms);
    if (ready_count < 0) {
        if (errno == EINTR) {
            return {};
        }
        ThrowLastSystemError("epoll_wait() failed");
    }

    std::vector<PollEvent> events;
    events.reserve(static_cast<std::size_t>(ready_count));
    for (int i = 0; i < ready_count; ++i) {
        events.push_back(PollEvent {
            ready_events_[i].data.fd,
            ready_events_[i].events,
        });
    }

    GrowReadyEventsIfNeeded(static_cast<std::size_t>(ready_count));
    return events;
}

// 返回 epoll fd 是否已可用。
bool EpollPoller::IsOpen() const noexcept {
    return epoll_fd_.valid();
}

// epoll_ctl 的统一入口。
// DEL 操作时 Linux 允许 event 参数为 nullptr。
void EpollPoller::Control(int operation, int fd, std::uint32_t events) {
    if (!epoll_fd_.valid()) {
        throw std::logic_error("epoll poller is not open");
    }

    epoll_event event {};
    event.events = events;
    event.data.fd = fd;

    if (::epoll_ctl(epoll_fd_.get(), operation, fd, operation == EPOLL_CTL_DEL ? nullptr : &event) != 0) {
        ThrowLastSystemError("epoll_ctl() failed");
    }
}

// 如果一轮事件数量正好等于当前缓冲区容量，
// 说明下一轮有可能继续被截断，所以把缓冲区直接翻倍。
void EpollPoller::GrowReadyEventsIfNeeded(std::size_t ready_count) {
    if (ready_count < ready_events_.size()) {
        return;
    }
    ready_events_.resize(ready_events_.size() * 2);
}

}  // namespace net
