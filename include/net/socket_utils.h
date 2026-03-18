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

// Day1 使用的最小监听闭环：socket -> SO_REUSEADDR -> bind -> listen。
UniqueFd CreateListenSocket(const std::string& host,
                            std::uint16_t port,
                            int backlog);

// 生成 "host:port" 形式的可读字符串，便于日志输出。
std::string DescribeEndpoint(const std::string& host, std::uint16_t port);

// 全局退出标记，供 Day1 的最小运行循环使用。
volatile std::sig_atomic_t& GlobalStopFlag() noexcept;

}  // namespace net
