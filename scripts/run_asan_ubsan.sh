#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-asan-ubsan"
DURATION_SEC="${SANITIZER_DURATION_SEC:-5}"

# Day4 先提供一个一键入口，把 asan_ubsan 配置、构建和核心自测串起来。
# 这里只跑当前已有的核心闭环，不提前接 wrk、证据归档或更长压测脚本。
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=ASanUBSan
cmake --build "${BUILD_DIR}" -j
ctest --test-dir "${BUILD_DIR}" --output-on-failure
python3 "${ROOT_DIR}/scripts/sanitizer_smoke.py" \
    --server-path "${BUILD_DIR}/bin/webserver" \
    --duration-sec "${DURATION_SEC}"
