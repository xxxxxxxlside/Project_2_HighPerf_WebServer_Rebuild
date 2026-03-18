# Project 2 High Performance WebServer Rebuild

本项目基于 `docs/mvp2.md` 与 `docs/RouteC_V23_9.md` 的要求，当前推进到 **Week1 Day3**：在 Day2 的非阻塞 accept 基础上，引入最小的 Epoll LT 事件循环。

## 当前进度

当前版本推进到 **Week1 Day3**：
- 完成项目初始化与目录结构整理。
- 完成 CMake 工程与基础脚本。
- 实现最小可运行闭环：`socket` 创建、`SO_REUSEADDR`、`bind`、`listen`、基础错误处理与启动流程。
- 把监听 socket 切换为非阻塞。
- 实现基础 accept 循环，能够持续接收新连接直到 `EAGAIN`。
- 引入 epoll，并把监听 fd 交给 Epoll LT 事件循环驱动。

## 当前版本做了什么

- `main.cpp` 只负责参数解析、创建 `Server`、启动服务。
- `Server` 负责监听 socket 初始化、切换非阻塞、注册 epoll、运行 Day3 事件循环。
- `socket_utils` 负责封装 `socket`、`setsockopt(SO_REUSEADDR)`、`bind`、`listen`、`fcntl(O_NONBLOCK)`、`accept4`。
- `epoll_poller` 负责封装 `epoll_create1`、`epoll_ctl`、`epoll_wait`。
- 当前只做到 Day3，不提前引入 connection、HTTP parser、response、buffer 等后续模块。

## 当前目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── docs/
├── include/
│   ├── core/
│   └── net/
├── results/
│   └── webserver/
├── scripts/
├── src/
│   ├── core/
│   └── net/
└── build/
```

## 模块说明

- `include/core` / `src/core`
  - `Server` 负责服务端启动与监听 socket 生命周期管理。
- `include/net` / `src/net`
  - socket 工具函数、fd RAII 封装、非阻塞设置、accept 封装和 epoll 封装。
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

- 已完成：工程初始化、模块拆分、监听启动、非阻塞监听 socket、基础 accept 循环、Epoll LT 监听事件循环。
- 未实现：connection 生命周期管理、读写、HTTP 解析、响应写回。

当前版本的目标仍然是保持简单、清晰、方便 review，只推进到文档要求的 **Week1 Day3**。
