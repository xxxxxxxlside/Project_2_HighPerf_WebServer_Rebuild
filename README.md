# Project 2 High Performance WebServer Rebuild

本项目基于 `docs/mvp2.md` 与 `docs/RouteC_V23_9.md` 的要求，当前推进到 **Week1 Day6**：在 Day5 的最小 HTTP 请求头解析基础上，补上静态 `200 OK` 响应、非阻塞写回以及 `outbuf + EPOLLOUT` 续写。

## 当前进度

当前版本推进到 **Week1 Day6**：
- 完成项目初始化与目录结构整理。
- 完成 CMake 工程与基础脚本。
- 实现最小可运行闭环：`socket` 创建、`SO_REUSEADDR`、`bind`、`listen`、基础错误处理与启动流程。
- 把监听 socket 切换为非阻塞。
- 实现基础 accept 循环，能够持续接收新连接直到 `EAGAIN`。
- 引入 epoll，并把监听 fd 交给 Epoll LT 事件循环驱动。
- 为每个连接维护最小输入缓冲区。
- 能按 `\r\n\r\n` 切出完整请求头，处理 TCP 粘包和拆包。
- 能解析 Request Line 和 Headers，并提取 `Content-Length`。
- 已实现 Day5 的协议边界防御：
  - `Chunked -> 501 + close`
  - `Header > 8192 -> 431 + close`
  - `POST` 缺少 `Content-Length -> 411 + close`
- 已实现 Day6 的基础响应写回：
  - 合法 GET/POST 返回静态 `200 OK`
  - 先尝试立即写回
  - 写不完时把剩余数据放入 `outbuf`
  - 通过 `EPOLLOUT` 在后续事件中继续发送

## 当前版本做了什么

- `main.cpp` 只负责参数解析、创建 `Server`、启动服务。
- `Server` 负责监听 socket 初始化、切换非阻塞、注册 epoll、维护最小连接表、读取数据、切分完整请求头、执行 Day5 解析和防御逻辑，并在 Day6 里接入 `outbuf + EPOLLOUT` 写回链路。
- `socket_utils` 负责封装 `socket`、`setsockopt(SO_REUSEADDR)`、`bind`、`listen`、`fcntl(O_NONBLOCK)`、`accept4`、best-effort 写回，以及 Day6 的单轮非阻塞写接口。
- `dynamic_buffer` 负责在应用层暂存字节流，并按 `\r\n\r\n` 找出完整请求头边界。
- `http_request` / `http_parser` / `http_response` 负责最小请求头解析、`200 OK` 响应拼装和错误响应拼装。
- `epoll_poller` 负责封装 `epoll_create1`、`epoll_ctl`、`epoll_wait`。
- 当前只做到 Day6，不提前进入 Week2 的 ReadyQueue、pending close queue、写预算、公平性控制和连接生命周期治理。

## 当前目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── docs/
├── include/
│   ├── core/
│   ├── http/
│   └── net/
├── results/
│   └── webserver/
├── scripts/
├── src/
│   ├── core/
│   ├── http/
│   └── net/
└── build/
```

## 模块说明

- `include/core` / `src/core`
  - `Server` 负责服务端启动、事件循环、连接表、读入数据、Day5 协议检查，以及 Day6 的静态响应写回。
- `include/net` / `src/net`
  - socket 工具函数、fd RAII 封装、非阻塞设置、accept 封装、动态缓冲区和 epoll 封装。
- `include/http` / `src/http`
  - 最小 HTTP 请求头解析、静态 `200 OK` 响应和错误响应拼装。
- `scripts`
  - 本地构建与演示脚本。
- `results/webserver`
  - 当前先保留结果目录，供后续手工运行与记录使用。

## 如何构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Release 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 如何运行

直接运行可执行文件：

```bash
./build/bin/webserver --port 8080 --host 0.0.0.0
```

或使用脚本：

```bash
./scripts/run_demo.sh --port 8080
```

## 当前版本边界

- 已完成：工程初始化、模块拆分、监听启动、非阻塞监听 socket、基础 accept 循环、Epoll LT 监听事件循环、应用层动态缓冲区、按 `\r\n\r\n` 处理 TCP 粘包 / 拆包、Request Line / Header 解析、`Content-Length` 提取、431 / 411 / 501 防御、静态 `200 OK` 写回、`outbuf + EPOLLOUT` 续写。
- 未实现：Week2 的 ReadyQueue、pending close queue、读写预算、公平性控制、Body 接收与超时治理。

当前版本的目标仍然是保持简单、清晰、方便 review，只推进到文档要求的 **Week1 Day6**。
