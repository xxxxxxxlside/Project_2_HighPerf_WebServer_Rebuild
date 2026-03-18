#include "net/socket_utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

// 把 errno 转成带上下文的异常，减少上层样板代码。
void ThrowLastSystemError(const char* message) {
    throw std::system_error(errno, std::generic_category(), message);
}

// 当前版本只处理 IPv4，后续若要扩展 IPv6，可以从这里继续抽象。
sockaddr_in BuildSockAddr(const std::string& host, std::uint16_t port) {
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        return addr;
    }

    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::invalid_argument("only IPv4 addresses are supported in Week1 Day1");
    }

    return addr;
}

}  // namespace

namespace net {

volatile std::sig_atomic_t& GlobalStopFlag() noexcept {
    static volatile std::sig_atomic_t stop_flag = 0;
    return stop_flag;
}

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

UniqueFd::~UniqueFd() {
    // 析构时统一 close，保证异常路径也不泄漏 fd。
    reset();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFd::get() const noexcept {
    return fd_;
}

bool UniqueFd::valid() const noexcept {
    return fd_ >= 0;
}

int UniqueFd::release() noexcept {
    // 释放所有权后把自身置空，避免重复 close。
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

UniqueFd CreateListenSocket(const std::string& host,
                            std::uint16_t port,
                            int backlog) {
    // Day1 的最小闭环就在这里收口，main/server 不再散落 socket 细节。
    UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listen_fd.valid()) {
        ThrowLastSystemError("socket() failed");
    }

    // 允许快速重启服务时复用地址，避免 TIME_WAIT 导致 bind 失败。
    const int reuse_addr = 1;
    if (::setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) != 0) {
        ThrowLastSystemError("setsockopt(SO_REUSEADDR) failed");
    }

    const sockaddr_in addr = BuildSockAddr(host, port);
    if (::bind(listen_fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ThrowLastSystemError("bind() failed");
    }

    if (::listen(listen_fd.get(), backlog) != 0) {
        ThrowLastSystemError("listen() failed");
    }

    return listen_fd;
}

std::string DescribeEndpoint(const std::string& host, std::uint16_t port) {
    // 输出日志时统一走这里，避免各处手拼 host:port。
    std::ostringstream oss;
    oss << (host.empty() ? "0.0.0.0" : host) << ':' << port;
    return oss.str();
}

}  // namespace net
