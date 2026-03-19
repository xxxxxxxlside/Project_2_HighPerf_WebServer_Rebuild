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
WRK_ROUNDS="${WRK_ROUNDS:-3}"
WRK_MAX_ATTEMPTS="${WRK_MAX_ATTEMPTS:-6}"

# [Week3 Day6] New:
# Day6 把 Release 构建、3 轮有效压测和证据目录落盘收口成一条 MVP 交付命令。
# 这里故意到此为止，不继续扩展到 Pro 或更复杂的 benchmark 编排。
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j
python3 "${ROOT_DIR}/scripts/mvp_acceptance.py" \
    --server-path "${BUILD_DIR}/bin/webserver" \
    --build-dir "${BUILD_DIR}" \
    --wrk-binary "${WRK_BINARY}" \
    --threads "${WRK_THREADS}" \
    --connections "${WRK_CONNECTIONS}" \
    --warmup-sec "${WRK_WARMUP_SEC}" \
    --measure-sec "${WRK_MEASURE_SEC}" \
    --reuse-threshold "${WRK_REUSE_THRESHOLD}" \
    --rounds "${WRK_ROUNDS}" \
    --max-attempts "${WRK_MAX_ATTEMPTS}"
