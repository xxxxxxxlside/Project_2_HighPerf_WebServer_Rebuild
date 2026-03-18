#pragma once

#include <csignal>
#include <cstdint>
#include <string>

namespace net {

// 最小 fd RAII 封装。
// 统一处理 close，避免后续连接对象和监听对象在异常路径上泄漏 fd。
class UniqueFd {
public:
    // 默认构造：表示当前对象还没有接管任何 fd。
    UniqueFd() = default;
    // 直接接管一个已经创建好的 fd。
    explicit UniqueFd(int fd) noexcept;
    // 析构时如果 fd 有效，会自动 close。
    ~UniqueFd();

    // 禁止拷贝，避免一个 fd 被多个对象重复 close。
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    // 允许移动，把 fd 所有权转移给新的对象。
    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    // 取出当前托管的原始 fd 值。
    [[nodiscard]] int get() const noexcept;
    // 判断当前是否真的托管着一个有效 fd。
    [[nodiscard]] bool valid() const noexcept;

    // 释放所有权但不 close，交给外部继续接管。
    int release() noexcept;
    // 重置托管 fd；若之前持有有效 fd，会先自动 close。
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

// Day2 开始需要把监听 socket 和新接入的连接 socket 设为非阻塞。
// 非阻塞的意义是：
// 当当前没有数据或没有新连接时，系统调用不会把线程卡死，而是立刻返回。
void SetNonBlocking(int fd);

// Day1 使用的最小监听闭环：socket -> SO_REUSEADDR -> bind -> listen。
UniqueFd CreateListenSocket(const std::string& host,
                            std::uint16_t port,
                            int backlog);

// 保存一次 accept 成功后拿到的结果。
// Day2 当前只需要知道：
// 1. 新连接的 fd
// 2. 对端地址字符串，便于日志输出和 review
struct AcceptedSocket {
    UniqueFd fd;
    std::string peer_endpoint;
};

// 尝试从监听 socket 上 accept 一个新连接。
// 返回值语义：
// - true  ：本次成功接到一个连接，结果写入 accepted。
// - false ：当前已经没有可接收的新连接了，通常表示遇到了 EAGAIN/EWOULDBLOCK。
//
// Day2 这里会把新连接直接设置为非阻塞，方便 Day3 以后继续往下接 epoll。
bool TryAcceptOne(int listen_fd, AcceptedSocket* accepted);

// 生成 "host:port" 形式的可读字符串，便于日志输出。
std::string DescribeEndpoint(const std::string& host, std::uint16_t port);

// 全局退出标记，供 Day1 的最小运行循环使用。
volatile std::sig_atomic_t& GlobalStopFlag() noexcept;

}  // namespace net
