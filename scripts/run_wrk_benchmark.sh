#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-release"
WRK_BINARY="${WRK_BINARY:-wrk}"
WRK_THREADS="${WRK_THREADS:-8}"
WRK_CONNECTIONS="${WRK_CONNECTIONS:-100}"
WRK_WARMUP_SEC="${WRK_WARMUP_SEC:-5}"
WRK_MEASURE_SEC="${WRK_MEASURE_SEC:-30}"
WRK_REUSE_THRESHOLD="${WRK_REUSE_THRESHOLD:-10}"

# [Week3 Day5] New:
# Day5 先把“Release 构建 + wrk 一轮验收 + 复用率校验”收口成一个脚本入口。
# 这里只打印本轮结果，不提前做 Day6 的多轮中位数和结果归档。
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j
python3 "${ROOT_DIR}/scripts/wrk_benchmark.py" \
    --server-path "${BUILD_DIR}/bin/webserver" \
    --wrk-binary "${WRK_BINARY}" \
    --threads "${WRK_THREADS}" \
    --connections "${WRK_CONNECTIONS}" \
    --warmup-sec "${WRK_WARMUP_SEC}" \
    --measure-sec "${WRK_MEASURE_SEC}" \
    --reuse-threshold "${WRK_REUSE_THRESHOLD}"
