#include "net/socket_utils.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
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

// 根据 host 和 port 组装 sockaddr_in。
// Day1 当前只支持 IPv4，所以这里直接返回 sockaddr_in。
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

// 把 accept 拿到的对端地址格式化成 "ip:port"。
// 这个字符串只用于日志和调试，不参与任何业务逻辑。
std::string DescribePeerEndpoint(const sockaddr_in& addr) {
    char ip_buffer[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &addr.sin_addr, ip_buffer, sizeof(ip_buffer)) == nullptr) {
        throw std::system_error(errno, std::generic_category(), "inet_ntop() failed");
    }

    std::ostringstream oss;
    oss << ip_buffer << ':' << ntohs(addr.sin_port);
    return oss.str();
}

}  // namespace

namespace net {

// 这里保存一个全局退出标记。
// main.cpp 在收到 SIGINT / SIGTERM 时会把它置 1，
// Server::Run() 通过轮询它来退出运行循环。
volatile std::sig_atomic_t& GlobalStopFlag() noexcept {
    static volatile std::sig_atomic_t stop_flag = 0;
    return stop_flag;
}

// 直接接管传进来的 fd。
UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

// 析构函数负责自动释放 fd。
UniqueFd::~UniqueFd() {
    // 析构时统一 close，保证异常路径也不泄漏 fd。
    reset();
}

// 移动构造：
// 从 other 手里拿走 fd，并把 other 置空。
UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

// 移动赋值：
// 先释放自己原来持有的 fd，再接管 other 的 fd。
UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

// 返回当前托管的原始 fd。
int UniqueFd::get() const noexcept {
    return fd_;
}

// 判断当前是否持有有效 fd。
bool UniqueFd::valid() const noexcept {
    return fd_ >= 0;
}

// 交出 fd 所有权，但不负责 close。
// 调用后当前对象会变成“空对象”状态。
int UniqueFd::release() noexcept {
    // 释放所有权后把自身置空，避免重复 close。
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

// 重置当前对象托管的 fd。
// 如果原来持有有效 fd，会先 close，再保存新的 fd。
void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

// 把一个已存在的 fd 切换成非阻塞模式。
// 之后像 accept/read/write 这类调用在“暂时做不了”时会返回 EAGAIN，
// 而不是把整个线程卡在系统调用里。
void SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        ThrowLastSystemError("fcntl(F_GETFL) failed");
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ThrowLastSystemError("fcntl(F_SETFL, O_NONBLOCK) failed");
    }
}

// 这是 Day1 最核心的工具函数。
// 它会一次性完成：
// 1. socket()
// 2. setsockopt(SO_REUSEADDR)
// 3. bind()
// 4. listen()
UniqueFd CreateListenSocket(const std::string& host,
                            std::uint16_t port,
                            int backlog) {
    // Day1 的最小闭环就在这里收口，main/server 不再散落 socket 细节。
    // 第一步：创建 TCP 监听 socket。
    UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listen_fd.valid()) {
        ThrowLastSystemError("socket() failed");
    }

    // 允许快速重启服务时复用地址，避免 TIME_WAIT 导致 bind 失败。
    // 第二步：打开 SO_REUSEADDR，方便服务重启时快速复用地址。
    const int reuse_addr = 1;
    if (::setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) != 0) {
        ThrowLastSystemError("setsockopt(SO_REUSEADDR) failed");
    }

    // 第三步：把地址和端口绑定到这个 socket 上。
    const sockaddr_in addr = BuildSockAddr(host, port);
    if (::bind(listen_fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ThrowLastSystemError("bind() failed");
    }

    // 第四步：把 socket 切换到监听状态。
    if (::listen(listen_fd.get(), backlog) != 0) {
        ThrowLastSystemError("listen() failed");
    }

    return listen_fd;
}

// Day2 的 accept 工具函数。
// 它每次只尝试接收一个连接，外层会在 Server 里循环调用它，
// 直到它返回 false，表示当前已经没有更多连接可接了。
bool TryAcceptOne(int listen_fd, AcceptedSocket* accepted) {
    if (accepted == nullptr) {
        throw std::invalid_argument("accepted must not be null");
    }

    sockaddr_in peer_addr {};
    socklen_t peer_addr_len = sizeof(peer_addr);

    // 这里使用 accept4，并直接给新连接加上 NONBLOCK 和 CLOEXEC。
    // 这样新连接从一开始就是非阻塞的，也避免 fd 在 exec 时被子进程继承。
    const int client_fd = ::accept4(listen_fd,
                                    reinterpret_cast<sockaddr*>(&peer_addr),
                                    &peer_addr_len,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        if (errno == EINTR) {
            return false;
        }
        ThrowLastSystemError("accept4() failed");
    }

    accepted->fd.reset(client_fd);
    accepted->peer_endpoint = DescribePeerEndpoint(peer_addr);
    return true;
}

// 尽量把一段小响应写出去。
// 由于当前 socket 是非阻塞的，所以 write() 可能在中途返回 EAGAIN；
// 一旦出现这种情况，当前阶段就直接视为“写不完”，交给上层 close。
bool WriteBestEffort(int fd, std::string_view data) {
    std::size_t offset = 0;

    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }

        return false;
    }

    return true;
}

// 把 host 和 port 拼成 "host:port" 字符串。
// 主要给日志输出使用。
std::string DescribeEndpoint(const std::string& host, std::uint16_t port) {
    // 输出日志时统一走这里，避免各处手拼 host:port。
    std::ostringstream oss;
    oss << (host.empty() ? "0.0.0.0" : host) << ':' << port;
    return oss.str();
}

}  // namespace net
