#include <csignal>
#include <cstdlib>
#include <exception>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "core/server.h"
#include "net/socket_utils.h"

namespace {

// 解析正整数。
// 这个函数给 --port 和 --backlog 共用，避免重复写转换逻辑。
int ParsePositiveInt(const std::string& value, const char* flag_name) {
    std::size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed);
    if (consumed != value.size() || parsed <= 0) {
        throw std::invalid_argument(std::string("invalid value for ") + flag_name + ": " + value);
    }
    return parsed;
}

// 专门解析端口号。
// 在正整数基础上，再额外校验端口必须落在 1-65535 范围内。
std::uint16_t ParsePort(const std::string& value) {
    const int port = ParsePositiveInt(value, "--port");
    if (port > 65535) {
        throw std::invalid_argument("port must be in range 1-65535");
    }
    return static_cast<std::uint16_t>(port);
}

// 信号处理函数。
// 当用户按 Ctrl+C 或系统发来终止信号时，
// 这里只做一件最小的事情：把全局退出标记设为 1。
void HandleSignal(int /*signal*/) {
    // 信号处理器只做最小动作：设置退出标记。
    net::GlobalStopFlag() = 1;
}

// 打印命令行帮助信息。
// 当用户传入 --help 或 -h 时，会调用这个函数。
void PrintUsage(const char* program_name) {
    std::cout
        << "Usage: " << program_name << " [options]\n"
        << "  --host <ipv4>      bind host, default 0.0.0.0\n"
        << "  --port <1-65535>   listen port, default 8080\n"
        << "  --backlog <n>      listen backlog, default 128\n"
        << "  --help, -h         show this help\n";
}

}  // namespace

// main 是程序入口。
// 它的职责只保留三件事：
// 1. 解析命令行参数。
// 2. 创建并初始化 Server。
// 3. 启动服务。
int main(int argc, char** argv) {
    try {
        // main 只保留启动职责：参数解析、创建 Server、启动服务。
        std::string host = "0.0.0.0";
        std::uint16_t port = 8080;
        int backlog = 128;

        // 从命令行读取用户传入的参数。
        // 如果用户不传，就使用上面定义的默认值。
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            // 这个小工具用于读取像 --port 8080 这种“带值参数”的值。
            auto require_value = [&](const char* flag_name) -> std::string {
                if (i + 1 >= argc) {
                    throw std::invalid_argument(std::string("missing value for ") + flag_name);
                }
                return argv[++i];
            };

            if (arg == "--host") {
                host = require_value("--host");
            } else if (arg == "--port") {
                port = ParsePort(require_value("--port"));
            } else if (arg == "--backlog") {
                backlog = ParsePositiveInt(require_value("--backlog"), "--backlog");
            } else if (arg == "--help" || arg == "-h") {
                PrintUsage(argv[0]);
                return EXIT_SUCCESS;
            } else {
                throw std::invalid_argument("unknown argument: " + arg);
            }
        }

        // 注册两个常见退出信号。
        // 这样按 Ctrl+C 时，程序可以走我们自己的退出流程。
        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);

        // 创建 Server 对象，并把解析好的参数传进去。
        core::Server server(host, port, backlog);
        // 做 Day1 所需的初始化：创建监听 socket。
        server.Initialize();
        // 启动运行循环，保持服务存活。
        server.Run();
    } catch (const std::exception& ex) {
        // 只要初始化或运行过程中抛出异常，就打印错误并返回失败码。
        std::cerr << "fatal: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }

    // 正常退出返回 0。
    return EXIT_SUCCESS;
}
