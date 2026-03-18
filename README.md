# Project 2 High Performance WebServer Rebuild

本项目基于 `docs/mvp2.md` 与 `docs/RouteC_V23_9.md` 的要求，只实现 **Week1 Day1**：完成一个可构建、可运行、可 review 的最小监听服务端工程。

## 当前进度

当前版本严格停留在 **Week1 Day1**：
- 完成项目初始化与目录结构整理。
- 完成 CMake 工程与基础脚本。
- 实现最小可运行闭环：`socket` 创建、`SO_REUSEADDR`、`bind`、`listen`、基础错误处理与启动流程。

## 当前版本做了什么

- `main.cpp` 只负责参数解析、创建 `Server`、启动服务。
- `Server` 负责监听 socket 初始化和最小运行循环。
- `socket_utils` 负责封装 `socket`、`setsockopt(SO_REUSEADDR)`、`bind`、`listen`。
- 只保留 Day1 真正需要的文件，不保留 epoll、connection、HTTP parser、response 等未来阶段代码。

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
  - socket 工具函数与 fd RAII 封装。
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

- 已完成：工程初始化、模块拆分、监听启动。
- 未实现：非阻塞 IO、accept 循环、epoll、connection、HTTP 解析、响应写回、测试。

这个版本的目标不是“为以后预留一切”，而是保持 **Week1 Day1** 极简、清晰、方便 review。
