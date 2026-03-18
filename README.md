# Project 2 High Performance WebServer Rebuild

## 项目简介

这是一个基于 `docs/mvp2.md` 和 `docs/RouteC_V23_9.md` 逐日重建的高性能 WebServer 练习项目。当前代码严格推进到 **Week1 Day6**，目标是先把单 Reactor、单 EventLoop、Epoll LT、非阻塞 IO、最小 HTTP 请求头解析和基础响应写回这条主链路走通。

当前版本不是“大量逻辑堆在 `main.cpp`” 的写法，而是按模块拆到 `core`、`net`、`http` 目录中，方便后续继续推进和做 code review。

## 当前进度

当前真实进度是 **Week1 Day6**。已经完成的内容如下：

- Day1：完成 `socket`、`SO_REUSEADDR`、`bind`、`listen` 的最小启动闭环。
- Day2：监听 socket 改为非阻塞，接入基础 `accept` 循环。
- Day3：引入 `epoll`，服务端改成单线程 Epoll LT 事件循环。
- Day4：加入应用层动态缓冲区，按 `\r\n\r\n` 处理 TCP 粘包 / 拆包。
- Day5：完成最小 HTTP 请求头解析，并实现三条协议边界防御：
  - `Transfer-Encoding: chunked -> 501 + close`
  - Header 超过 `8192` 字节 -> `431 + close`
  - `POST` 缺少 `Content-Length -> 411 + close`
- Day6：对合法 `GET / POST` 返回静态 `200 OK`，并补上基础响应写回：
  - 先尝试立即写回
  - 若没写完，把剩余数据放进 `outbuf`
  - 注册 `EPOLLOUT`，等待下次可写时继续发送

## 当前已经实现的功能

- 命令行启动服务：`main.cpp` 只负责参数解析、创建 `Server`、启动运行。
- 监听 socket 初始化：
  - `socket`
  - `setsockopt(SO_REUSEADDR)`
  - `bind`
  - `listen`
- 非阻塞 IO：
  - 监听 socket 非阻塞
  - 新接入连接通过 `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)` 直接设为非阻塞
- 单 Reactor 单 EventLoop：
  - 使用 Epoll LT
  - 监听 fd 和客户端 fd 都由同一线程处理
- 应用层输入缓冲：
  - 支持多次 `read()` 拼出完整请求头
  - 支持一次 `read()` 中切出多个完整请求头
- 最小 HTTP 请求头解析：
  - 解析 Request Line：Method / URI / Version
  - 解析 Headers
  - 提取 `Content-Length`
- Day5 协议边界防御：
  - `431 / 411 / 501` 错误响应
  - best-effort 写回后关闭连接
- Day6 基础响应写回：
  - 合法请求返回静态 `200 OK`
  - 支持 `outbuf + EPOLLOUT` 继续发送未写完的响应

## 项目目录结构

当前项目目录和代码文件如下：

```text
.
├── CMakeLists.txt
├── README.md
├── docs
│   ├── RouteC_V23_9.md
│   ├── RouteC_V23_9.pdf
│   ├── mvp2.md
│   └── mvp2.pdf
├── include
│   ├── core
│   │   └── server.h
│   ├── http
│   │   ├── http_parser.h
│   │   ├── http_request.h
│   │   └── http_response.h
│   └── net
│       ├── dynamic_buffer.h
│       ├── epoll_poller.h
│       └── socket_utils.h
├── results
│   └── webserver
├── scripts
│   └── run_demo.sh
└── src
    ├── CMakeLists.txt
    ├── core
    │   └── server.cpp
    ├── http
    │   ├── http_parser.cpp
    │   ├── http_request.cpp
    │   └── http_response.cpp
    ├── main.cpp
    └── net
        ├── dynamic_buffer.cpp
        ├── epoll_poller.cpp
        └── socket_utils.cpp
```

## 模块说明

- `src/main.cpp`
  - 启动入口，只做参数读取、信号处理和 `Server` 组装。
- `include/core` / `src/core`
  - `Server` 负责事件循环、接收连接、读取数据、解析请求头、协议检查、响应写回。
- `include/net` / `src/net`
  - 封装 fd RAII、socket 初始化、非阻塞设置、`accept4`、动态缓冲区和 epoll。
- `include/http` / `src/http`
  - 封装最小 HTTP 请求对象、请求头解析器和响应拼装逻辑。
- `scripts/run_demo.sh`
  - 提供本地构建和运行演示脚本。
- `results/webserver`
  - 预留给后续测试结果、压测记录和日志文件。

## 构建方法

Debug 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Release 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 运行方法

直接运行：

```bash
./build/bin/webserver --host 0.0.0.0 --port 8080
```

使用脚本运行：

```bash
./scripts/run_demo.sh --port 8080
```

## 当前边界

当前版本**已经做到**：

- 单 Reactor 单 EventLoop
- Epoll LT + 非阻塞 IO
- 动态输入缓冲
- HTTP 请求头最小解析
- `431 / 411 / 501` 错误响应
- 合法请求静态 `200 OK`
- `outbuf + EPOLLOUT` 的基础续写链路

当前版本**还没有做到**：

- Week2 的 ReadyQueue
- 每轮读写预算控制
- pending close queue
- Body 接收与超时治理
- 全局 inflight budget
- IP 限流和连接数封顶
- Week3 的 metrics、定时器、压测与验收落盘

## 后续计划

后续将按文档继续推进，但这些内容现在**还没有实现**：

- Week1 Day7：复盘当前 Epoll LT、非阻塞 IO、`inbuf / outbuf` 的工作机制。
- Week2：引入 ReadyQueue、读写预算、连接关闭统一入口、内存预算和限流约束。
- Week3：补齐超时控制、指标打点、压测脚本和验收结果落盘。
