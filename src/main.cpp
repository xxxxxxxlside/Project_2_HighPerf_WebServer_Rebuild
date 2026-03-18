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

int ParsePositiveInt(const std::string& value, const char* flag_name) {
    std::size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed);
    if (consumed != value.size() || parsed <= 0) {
        throw std::invalid_argument(std::string("invalid value for ") + flag_name + ": " + value);
    }
    return parsed;
}

std::uint16_t ParsePort(const std::string& value) {
    const int port = ParsePositiveInt(value, "--port");
    if (port > 65535) {
        throw std::invalid_argument("port must be in range 1-65535");
    }
    return static_cast<std::uint16_t>(port);
}

void HandleSignal(int /*signal*/) {
    // 信号处理器只做最小动作：设置退出标记。
    net::GlobalStopFlag() = 1;
}

void PrintUsage(const char* program_name) {
    std::cout
        << "Usage: " << program_name << " [options]\n"
        << "  --host <ipv4>      bind host, default 0.0.0.0\n"
        << "  --port <1-65535>   listen port, default 8080\n"
        << "  --backlog <n>      listen backlog, default 128\n"
        << "  --help, -h         show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // main 只保留启动职责：参数解析、创建 Server、启动服务。
        std::string host = "0.0.0.0";
        std::uint16_t port = 8080;
        int backlog = 128;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

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

        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);

        core::Server server(host, port, backlog);
        server.Initialize();
        server.Run();
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
