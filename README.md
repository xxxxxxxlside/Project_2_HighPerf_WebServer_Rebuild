# Project 2 High Performance WebServer Rebuild

## 项目简介

这是一个基于 `docs/mvp2.md` 和 `docs/RouteC_V23_9.md` 逐日重建的高性能 WebServer 练习项目。当前代码已经推进到 **Week2 Day6**，目标是在保留现有单 Reactor、单 EventLoop 结构的前提下，把连接管理、资源约束和拒绝语义逐步补齐。

当前版本不是“大量逻辑堆在 `main.cpp`” 的写法，而是按模块拆到 `core`、`net`、`http` 目录中，方便后续继续推进和做 code review。

## 当前进度

当前真实进度是 **Week2 Day6**。已经完成的内容如下：

- Week1 Day1-Day6：完成监听 socket、非阻塞 `accept`、Epoll LT 事件循环、动态输入缓冲、最小 HTTP 请求头解析、`431 / 411 / 501` 协议边界防御，以及 `200 OK + outbuf + EPOLLOUT` 的基础写回闭环。
- Week2 Day1：加入 ReadyQueue，避免“用户态 buffer 里已有完整请求，但内核不再触发新 EPOLLIN”时出现 LT 假死。
- Week2 Day2：补上单连接单轮处理上限，以及单轮读写预算 `256KB`，避免单连接长时间独占 EventLoop。
- Week2 Day3：统一连接关闭入口，接入 `closing=true`、ReadyQueue 摘除和 `pending_close_queue` 延迟释放路径。
- Week2 Day4：补上 `Content-Length > 8MB -> 413 + close` 和 `global_inflight_bytes` 预算检查。
- Week2 Day5：在 accept 路径接入按 IP 计数的 token bucket，支持 TTL 和 LRU 淘汰。
- Week2 Day6：加入全局 `max_conns = 200k` 封顶；超限连接会复用唯一关闭入口，并递增 `conn_reject_total`。

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
  - `Content-Length > 8MB -> 413 + close`
  - best-effort 写回后关闭连接
- Day6 基础响应写回：
  - 合法请求返回静态 `200 OK`
  - 支持 `outbuf + EPOLLOUT` 继续发送未写完的响应
- Week2 连接与资源约束：
  - ReadyQueue 续处理，避免 LT 模式下因处理上限导致假死
  - 单轮读写预算 `256KB`
  - 统一关闭入口 + `pending_close_queue` 延迟释放
  - `global_inflight_bytes` 内存预算
  - 按 IP 的 token bucket 限流
  - 全局 `max_conns = 200k` 连接数封顶与 `conn_reject_total` 计数

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
- `431 / 411 / 501 / 413` 错误响应
- 合法请求静态 `200 OK`
- `outbuf + EPOLLOUT` 的基础续写链路
- ReadyQueue、每轮读写预算、唯一关闭入口、`pending_close_queue`
- `global_inflight_bytes` 内存预算
- IP 限流和 `max_conns = 200k` 连接数封顶

当前版本**还没有做到**：

- 真正的 Body 接收状态机与 body 超时治理
- Week2 Day7 的复盘和文档化梳理
- Week3 的 metrics、定时器、压测与验收落盘

## 后续计划

后续将按文档继续推进，但这些内容现在**还没有实现**：

- Week2 Day7：复盘当前 Epoll LT、非阻塞 IO、ReadyQueue、连接关闭路径和计数口径。
- Week3：补齐超时控制、指标打点、压测脚本和验收结果落盘。
