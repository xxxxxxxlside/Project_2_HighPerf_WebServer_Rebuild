#pragma once

#include <csignal>
#include <cstdint>
#include <string>

namespace net {

// 最小 fd RAII 封装。
// 统一处理 close，避免后续连接对象和监听对象在异常路径上泄漏 fd。
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept;
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

    // 释放所有权但不 close，交给外部继续接管。
    int release() noexcept;
    // 重置托管 fd；若之前持有有效 fd，会先自动 close。
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

// Day1 使用的最小监听闭环：socket -> SO_REUSEADDR -> bind -> listen。
UniqueFd CreateListenSocket(const std::string& host,
                            std::uint16_t port,
                            int backlog);

// 生成 "host:port" 形式的可读字符串，便于日志输出。
std::string DescribeEndpoint(const std::string& host, std::uint16_t port);

// 全局退出标记，供 Day1 的最小运行循环使用。
volatile std::sig_atomic_t& GlobalStopFlag() noexcept;

}  // namespace net
