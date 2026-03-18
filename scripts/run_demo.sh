#!/usr/bin/env bash

# 遇到错误立刻退出；
# 未定义变量直接报错；
# 管道中任一命令失败都算失败。
set -euo pipefail

# 计算仓库根目录路径。
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 本地构建目录。
BUILD_DIR="${ROOT_DIR}/build"
# 如果外部没传 CMAKE_BUILD_TYPE，就默认用 Debug。
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"

# 第一步：生成构建文件。
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
# 第二步：编译项目。
cmake --build "${BUILD_DIR}" -j

# 第三步：启动编译出来的可执行文件。
exec "${BUILD_DIR}/bin/webserver" "$@"
