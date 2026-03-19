#!/usr/bin/env python3

import argparse
from dataclasses import dataclass
import datetime as dt
import os
import pathlib
import re
import socket
import subprocess
import sys
from typing import Iterable

import wrk_benchmark


@dataclass
class RoundRecord:
    attempt_index: int
    valid: bool
    invalid_reason: str | None
    result: wrk_benchmark.BenchmarkRunResult


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Finalize the Project2 MVP evidence for Week3 Day6."
    )
    parser.add_argument("--server-path", required=True, help="Path to the webserver binary")
    parser.add_argument("--build-dir", required=True, help="Path to the Release build directory")
    parser.add_argument("--wrk-binary", default="wrk", help="Path to the wrk executable")
    parser.add_argument("--host", default="127.0.0.1", help="Server bind host")
    parser.add_argument("--port", type=int, default=18080, help="Server bind port")
    parser.add_argument("--path", default="/", help="HTTP path to benchmark")
    parser.add_argument("--threads", type=int, default=8, help="wrk thread count")
    parser.add_argument("--connections", type=int, default=100, help="wrk connection count")
    parser.add_argument("--warmup-sec", type=int, default=5, help="Warmup duration in seconds")
    parser.add_argument("--measure-sec", type=int, default=30, help="Measurement duration in seconds")
    parser.add_argument("--reuse-threshold", type=float, default=10.0)
    parser.add_argument("--rounds", type=int, default=3, help="How many valid rounds are required")
    parser.add_argument(
        "--max-attempts",
        type=int,
        default=6,
        help="Maximum total attempts before giving up on collecting valid rounds",
    )
    return parser.parse_args()


def hostname_for_results() -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "_", socket.gethostname() or "unknown")


def build_results_dir(project_root: pathlib.Path) -> pathlib.Path:
    date_prefix = dt.datetime.now().strftime("%Y%m%d")
    return project_root / "results" / "webserver" / f"{date_prefix}_{hostname_for_results()}"


def run_command(command: list[str]) -> str:
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed with code {completed.returncode}: {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    return completed.stdout


def read_build_flags(build_dir: pathlib.Path) -> str:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.exists():
        raise RuntimeError(f"CMakeCache.txt not found in build dir: {build_dir}")

    interesting_prefixes = (
        "CMAKE_BUILD_TYPE:",
        "CMAKE_CXX_COMPILER:",
        "CMAKE_CXX_FLAGS:",
        "CMAKE_CXX_FLAGS_RELEASE:",
        "CMAKE_EXE_LINKER_FLAGS:",
        "CMAKE_EXE_LINKER_FLAGS_RELEASE:",
    )
    lines = [
        line
        for line in cache_path.read_text(encoding="utf-8").splitlines()
        if line.startswith(interesting_prefixes)
    ]

    env_cxxflags = f"CXXFLAGS={os.environ.get('CXXFLAGS', '')}"
    env_ldflags = f"LDFLAGS={os.environ.get('LDFLAGS', '')}"
    return "\n".join(lines + [env_cxxflags, env_ldflags]) + "\n"


def extract_percentile_ms(wrk_stdout: str, percentile: int) -> str:
    pattern = re.compile(rf"^\s*{percentile}%\s+([0-9.]+)([a-zA-Z]+)\s*$", re.MULTILINE)
    match = pattern.search(wrk_stdout)
    if match is None:
        return "N/A"

    value = float(match.group(1))
    unit = match.group(2).lower()
    if unit == "us":
        return f"{value / 1000.0:.3f}ms"
    if unit == "ms":
        return f"{value:.3f}ms"
    if unit == "s":
        return f"{value * 1000.0:.3f}ms"
    return "N/A"


def round_row(record: RoundRecord) -> str:
    reuse_text = "invalid" if record.result.reuse_ratio is None else f"{record.result.reuse_ratio:.4f}"
    validity = "valid" if record.valid else f"invalid ({record.invalid_reason})"
    return (
        f"| {record.attempt_index} | {validity} | {record.result.accept_delta} | "
        f"{record.result.requests_delta} | {record.result.qps:.2f} | {reuse_text} | "
        f"{record.result.rss_delta_kb} | {record.result.cpu_pct:.2f} |"
    )


def choose_median_round(valid_rounds: Iterable[RoundRecord]) -> RoundRecord:
    sorted_rounds = sorted(valid_rounds, key=lambda record: record.result.qps)
    return sorted_rounds[len(sorted_rounds) // 2]


def main() -> int:
    args = parse_args()
    project_root = pathlib.Path(__file__).resolve().parent.parent
    build_dir = pathlib.Path(args.build_dir).resolve()
    results_dir = build_results_dir(project_root)
    results_dir.mkdir(parents=True, exist_ok=True)

    records: list[RoundRecord] = []
    valid_rounds: list[RoundRecord] = []

    # [Week3 Day6] Begin:
    # Day6 的收尾目标是把 Day5 的“单轮 benchmark”升级成 MVP 交付证据：
    # 1. 连续跑 benchmark，直到收集到 3 轮有效结果
    # 2. 遇到 accept_delta == 0 的无效轮时丢弃并补跑
    # 3. 把每轮原始 wrk 输出和最终中位数结论落到 results/webserver/<date>_<machine>/
    #
    # 这里把“有效轮”的定义固定为：
    # - measurement window 内 accept_delta > 0
    # - wrk 进程本身执行成功
    # 至于 reuse_ratio 是否达标，则留到拿到 3 轮有效结果后统一做中位数判断。
    benchmark_args = argparse.Namespace(
        server_path=args.server_path,
        wrk_binary=args.wrk_binary,
        host=args.host,
        port=args.port,
        path=args.path,
        threads=args.threads,
        connections=args.connections,
        warmup_sec=args.warmup_sec,
        measure_sec=args.measure_sec,
        reuse_threshold=args.reuse_threshold,
    )

    for attempt_index in range(1, args.max_attempts + 1):
        result = wrk_benchmark.run_benchmark_once(benchmark_args)
        if result.reuse_ratio is None:
            records.append(
                RoundRecord(
                    attempt_index=attempt_index,
                    valid=False,
                    invalid_reason="accept_delta == 0",
                    result=result,
                )
            )
            continue

        record = RoundRecord(
            attempt_index=attempt_index,
            valid=True,
            invalid_reason=None,
            result=result,
        )
        records.append(record)
        valid_rounds.append(record)
        if len(valid_rounds) >= args.rounds:
            break

    if len(valid_rounds) < args.rounds:
        raise RuntimeError(
            f"only collected {len(valid_rounds)} valid rounds after {args.max_attempts} attempts"
        )

    median_round = choose_median_round(valid_rounds)
    if median_round.result.reuse_ratio is None:
        raise RuntimeError("median round unexpectedly has no reuse ratio")

    if median_round.result.reuse_ratio <= args.reuse_threshold:
        raise RuntimeError(
            f"median reuse ratio check failed: {median_round.result.reuse_ratio:.4f} <= "
            f"{args.reuse_threshold:.4f}"
        )
    # [Week3 Day6] End

    machine_txt = (
        "=== uname -a ===\n"
        f"{run_command(['uname', '-a']).rstrip()}\n\n"
        "=== lscpu ===\n"
        f"{run_command(['lscpu']).rstrip()}\n\n"
        "=== /proc/meminfo (MemTotal/MemAvailable) ===\n"
        + "\n".join(
            line
            for line in pathlib.Path("/proc/meminfo").read_text(encoding="utf-8").splitlines()
            if line.startswith(("MemTotal:", "MemAvailable:"))
        )
        + "\n"
    )
    (results_dir / "machine.txt").write_text(machine_txt, encoding="utf-8")

    (results_dir / "build_flags.txt").write_text(read_build_flags(build_dir), encoding="utf-8")

    bench_cmd_lines = [
        f"server_path={pathlib.Path(args.server_path).resolve()}",
        f"wrk_binary={args.wrk_binary}",
        f"host={args.host}",
        f"port={args.port}",
        f"path={args.path}",
        f"threads={args.threads}",
        f"connections={args.connections}",
        f"warmup_sec={args.warmup_sec}",
        f"measure_sec={args.measure_sec}",
        f"reuse_threshold={args.reuse_threshold}",
        f"rounds={args.rounds}",
        f"max_attempts={args.max_attempts}",
        f"single_round_command={' '.join(median_round.result.wrk_command)}",
    ]
    (results_dir / "bench_cmd.txt").write_text("\n".join(bench_cmd_lines) + "\n", encoding="utf-8")

    latency_parts: list[str] = []
    for record in records:
        latency_parts.append(
            "\n".join(
                [
                    f"=== attempt {record.attempt_index} ===",
                    f"valid={str(record.valid).lower()}",
                    f"invalid_reason={record.invalid_reason or 'none'}",
                    f"accept_delta={record.result.accept_delta}",
                    f"requests_delta={record.result.requests_delta}",
                    f"qps={record.result.qps:.4f}",
                    f"reuse_ratio={'N/A' if record.result.reuse_ratio is None else f'{record.result.reuse_ratio:.4f}'}",
                    "--- wrk stdout ---",
                    record.result.wrk_stdout.rstrip(),
                    "--- wrk stderr ---",
                    record.result.wrk_stderr.rstrip(),
                    "",
                ]
            )
        )
    (results_dir / "latency_samples.txt").write_text("\n".join(latency_parts), encoding="utf-8")

    summary_lines = [
        "# Project2 MVP Summary",
        "",
        f"- status: PASS",
        f"- valid_rounds: {len(valid_rounds)}/{args.rounds}",
        f"- attempts_total: {len(records)}",
        f"- median_round_attempt: {median_round.attempt_index}",
        f"- reuse_threshold: {args.reuse_threshold:.4f}",
        "",
        "## Round Table",
        "",
        "| attempt | validity | accept_delta | requests_delta | qps | reuse_ratio | rss_delta_kb | cpu_pct |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    summary_lines.extend(round_row(record) for record in records)
    summary_lines.extend(
        [
            "",
            "## Median Round",
            "",
            f"- qps: {median_round.result.qps:.4f}",
            f"- accept_delta: {median_round.result.accept_delta}",
            f"- requests_delta: {median_round.result.requests_delta}",
            f"- reject_delta: {median_round.result.reject_delta}",
            f"- errors_delta: {median_round.result.errors_delta}",
            f"- reuse_ratio: {median_round.result.reuse_ratio:.4f}",
            f"- rss_delta_kb: {median_round.result.rss_delta_kb}",
            f"- cpu_pct: {median_round.result.cpu_pct:.4f}",
            f"- p50: {extract_percentile_ms(median_round.result.wrk_stdout, 50)}",
            f"- p95: {extract_percentile_ms(median_round.result.wrk_stdout, 95)}",
            f"- p99: {extract_percentile_ms(median_round.result.wrk_stdout, 99)}",
            "",
            "## Notes",
            "",
            "- `qps` 使用 measurement window 内 `requests_total` 差分除以窗口秒数计算。",
            "- `reuse_ratio` 使用 measurement window 内 `requests_total / accept_total` 差分计算。",
            "- `latency_samples.txt` 保存每轮原始 `wrk --latency` 输出，便于后续人工复核。",
        ]
    )
    (results_dir / "summary.md").write_text("\n".join(summary_lines) + "\n", encoding="utf-8")

    print(f"results_dir={results_dir}")
    print(f"median_round_attempt={median_round.attempt_index}")
    print(f"median_qps={median_round.result.qps:.4f}")
    print(f"median_reuse_ratio={median_round.result.reuse_ratio:.4f}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"fatal: {exc}", file=sys.stderr)
        raise SystemExit(1)
