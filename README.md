# Project 2 High Performance WebServer Rebuild

## 项目概览

这是一个按 `docs/mvp2.md` 与 `docs/RouteC_V23_9.md` 逐日推进的 C++ WebServer 重建项目。当前实现坚持保留现有的单 Reactor、单 EventLoop 架构，不推翻已有模块拆分，只在当前代码基础上继续补边界、生命周期和可观测性前置能力。

当前目录按职责拆分为：

- `src/main.cpp`：进程入口、参数解析、信号注册。
- `include/core` / `src/core`：`Server` 事件循环、连接管理、协议边界、防御逻辑。
- `include/http` / `src/http`：最小 HTTP 请求头解析与文本响应拼装。
- `include/net` / `src/net`：socket、epoll、动态缓冲区和 fd 封装。
- `scripts/run_demo.sh`：本地构建并启动服务。

## 当前进度

当前真实进度是 **Week3 Day1**。

已完成：

- Week1 Day1-Day6：监听 socket、非阻塞 accept、Epoll LT、动态输入缓冲、最小 HTTP 请求头解析、`431 / 411 / 501` 防御、静态 `200 OK` 响应写回。
- Week2 Day1-Day6：ReadyQueue、防 LT 假死、单轮读写预算、唯一关闭入口、`pending_close_queue` 延迟释放、`global_inflight_bytes`、`8MB body limit`、IP token bucket、`max_conns = 200k`。
- Week3 Day1：最小堆定时器基础、`header_timeout = 10s`、`idle_keepalive_timeout = 60s`，并把 keep-alive 安全地限制在“当前代码能正确处理的无 body 请求”范围内。

## 当前能力

- 单 Reactor 单 EventLoop，监听 fd 和客户端 fd 由同一线程串行处理。
- Epoll LT + 非阻塞 IO。
- 动态输入缓冲，支持多次 `read()` 拼接完整 header，也支持一次读取中拆出多个完整请求头。
- HTTP/1.0 / HTTP/1.1 的最小请求头解析。
- `431 / 411 / 413 / 501 / 503 / 429` 等边界拒绝响应。
- 基础 `200 OK` 写回，支持 `outbuf + EPOLLOUT` 继续发送未刷完的响应。
- ReadyQueue、读写预算、唯一关闭入口、`pending_close_queue` 延迟释放。
- `global_inflight_bytes` 内存预算、IP 限流、活跃连接数封顶。
- Week3 Day1 定时器：
  - 新连接或请求头接收中连接会挂 `header_timeout`。
  - keep-alive 空闲连接会挂 `idle_keepalive_timeout`。

## 当前边界

本版本仍然**没有**实现下面这些内容：

- 真正的 request body 接收状态机。
- Week3 Day2 之后的 body deadline / body timeout。
- Week3 Day3 之后的 metrics 日志、压测结果落盘和验收证据目录。
- Sanitizer、wrk 压测和结果归档脚本化。

因此当前 keep-alive 的收口策略是：

- 只有无 body 请求才允许保留连接。
- 只要请求声明了非零 `Content-Length`，仍然走现有 close 语义，避免未消费 body 污染后续请求边界。

## 构建与运行

Debug:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Release:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

直接运行：

```bash
./build/bin/webserver --host 0.0.0.0 --port 8080
```

或使用脚本：

```bash
./scripts/run_demo.sh --port 8080
```

## 下一步

如果继续按计划推进，下一阶段应该只做 **Week3 Day2**：

- 在当前定时器基础上接 body deadline 计算。
- 为慢速 body 传输补 `body_timeout` 上限截断。
- 继续保持不推翻现有 `Server` 结构。
