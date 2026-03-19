#!/usr/bin/env python3

import argparse
from dataclasses import dataclass
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import time


@dataclass
class BenchmarkRunResult:
    wrk_command: list[str]
    wrk_stdout: str
    wrk_stderr: str
    metrics_log_path: pathlib.Path
    warmup_sample_ts_ms: int
    end_sample_ts_ms: int
    accept_delta: int
    requests_delta: int
    reject_delta: int
    errors_delta: int
    reuse_ratio: float | None
    qps: float
    rss_delta_kb: int
    cpu_pct: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the Week3 Day5 wrk benchmark and verify the keep-alive reuse ratio."
    )
    parser.add_argument("--server-path", required=True, help="Path to the webserver binary")
    parser.add_argument("--wrk-binary", default="wrk", help="Path to the wrk executable")
    parser.add_argument("--host", default="127.0.0.1", help="Server bind host")
    parser.add_argument("--port", type=int, default=18080, help="Server bind port")
    parser.add_argument("--path", default="/", help="HTTP path to benchmark")
    parser.add_argument("--threads", type=int, default=8, help="wrk thread count")
    parser.add_argument("--connections", type=int, default=100, help="wrk connection count")
    parser.add_argument("--warmup-sec", type=int, default=5, help="Warmup duration in seconds")
    parser.add_argument("--measure-sec", type=int, default=30, help="Measurement duration in seconds")
    parser.add_argument(
        "--reuse-threshold",
        type=float,
        default=10.0,
        help="Minimum allowed requests_total / accept_total reuse ratio",
    )
    return parser.parse_args()


def wait_for_server_ready(host: str, port: int, timeout_sec: float) -> None:
    deadline = time.monotonic() + timeout_sec
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)

    raise RuntimeError(f"server did not become ready: {last_error}")


def find_latest_metrics_log(project_root: pathlib.Path) -> pathlib.Path:
    candidates = list(project_root.glob("results/webserver/*/metrics.log"))
    if not candidates:
        raise RuntimeError("metrics.log was not created")
    return max(candidates, key=lambda path: path.stat().st_mtime_ns)


def read_metrics_samples(log_path: pathlib.Path) -> list[dict[str, int]]:
    samples: list[dict[str, int]] = []
    for raw_line in log_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line:
            continue

        sample: dict[str, int] = {}
        for token in line.split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            sample[key] = int(value)

        if "ts" in sample:
            samples.append(sample)

    return samples


def wait_for_metrics_sample(log_path: pathlib.Path, target_ts_ms: int, timeout_sec: float) -> list[dict[str, int]]:
    deadline = time.monotonic() + timeout_sec
    latest_samples: list[dict[str, int]] = []
    while time.monotonic() < deadline:
        latest_samples = read_metrics_samples(log_path)
        if any(sample["ts"] >= target_ts_ms for sample in latest_samples):
            return latest_samples
        time.sleep(0.1)

    raise RuntimeError(
        f"metrics.log did not reach target timestamp {target_ts_ms}: "
        f"latest_sample={(latest_samples[-1] if latest_samples else None)}"
    )


def sample_at_or_after(samples: list[dict[str, int]], target_ts_ms: int) -> dict[str, int]:
    for sample in samples:
        if sample["ts"] >= target_ts_ms:
            return sample

    raise RuntimeError(f"no metrics sample found at or after {target_ts_ms}")


def format_wrk_url(host: str, port: int, path: str) -> str:
    normalized_path = path if path.startswith("/") else f"/{path}"
    return f"http://{host}:{port}{normalized_path}"


def read_process_cpu_total_seconds(pid: int) -> float:
    stat_text = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    stat_tail = stat_text.split(") ", 1)[1].split()
    utime_ticks = int(stat_tail[11])
    stime_ticks = int(stat_tail[12])
    clock_ticks_per_second = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    return (utime_ticks + stime_ticks) / clock_ticks_per_second


def run_benchmark_once(args: argparse.Namespace) -> BenchmarkRunResult:
    project_root = pathlib.Path(__file__).resolve().parent.parent
    server_path = pathlib.Path(args.server_path).resolve()
    if not server_path.exists():
        raise RuntimeError(f"server binary does not exist: {server_path}")

    if args.warmup_sec <= 0 or args.measure_sec <= 0:
        raise RuntimeError("warmup-sec and measure-sec must both be positive")

    total_duration_sec = args.warmup_sec + args.measure_sec
    wrk_url = format_wrk_url(args.host, args.port, args.path)

    # Day5 的目标是把 wrk 压测口径和 metrics 差分统计固化成一条脚本：
    # 1. 用 wrk 按固定参数发起 keep-alive 压测
    # 2. 从 metrics.log 里取 warmup 结束和测量结束两个采样点
    # 3. 用 requests_total / accept_total 的差分值校验复用率
    #
    # 这里故意只做“跑一轮 + 当场验收打印”，不提前做 Day6 的 summary、三轮中位数和证据落盘。
    with tempfile.NamedTemporaryFile(mode="w+", encoding="utf-8", delete=False) as server_log:
        server_process = subprocess.Popen(
            [
                str(server_path),
                "--host",
                args.host,
                "--port",
                str(args.port),
            ],
            cwd=project_root,
            stdout=server_log,
            stderr=subprocess.STDOUT,
            text=True,
        )

        try:
            wait_for_server_ready(args.host, args.port, timeout_sec=5.0)
            metrics_log_path = find_latest_metrics_log(project_root)

            run_start_ms = int(time.time() * 1000)
            warmup_boundary_ms = run_start_ms + args.warmup_sec * 1000
            end_boundary_ms = run_start_ms + total_duration_sec * 1000

            wrk_command = [
                args.wrk_binary,
                f"-t{args.threads}",
                f"-c{args.connections}",
                f"-d{total_duration_sec}s",
                "--latency",
                wrk_url,
            ]

            try:
                wrk_process = subprocess.Popen(
                    wrk_command,
                    cwd=project_root,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            except FileNotFoundError as exc:
                raise RuntimeError(
                    f"wrk executable was not found: {args.wrk_binary}. "
                    "Install wrk or pass --wrk-binary <path>."
                ) from exc

            time.sleep(args.warmup_sec)
            cpu_start_seconds = read_process_cpu_total_seconds(server_process.pid)
            cpu_wall_start = time.monotonic()
            wrk_stdout, wrk_stderr = wrk_process.communicate()
            cpu_wall_seconds = time.monotonic() - cpu_wall_start
            cpu_end_seconds = read_process_cpu_total_seconds(server_process.pid)

            if wrk_process.returncode != 0:
                raise RuntimeError(
                    f"wrk failed with code {wrk_process.returncode}\n"
                    f"stdout:\n{wrk_stdout}\n"
                    f"stderr:\n{wrk_stderr}"
                )

            samples = wait_for_metrics_sample(metrics_log_path, end_boundary_ms, timeout_sec=5.0)
            warmup_sample = sample_at_or_after(samples, warmup_boundary_ms)
            end_sample = sample_at_or_after(samples, end_boundary_ms)
        finally:
            server_process.send_signal(signal.SIGINT)
            try:
                server_process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                server_process.kill()
                server_process.wait(timeout=5.0)

        server_log.flush()
        server_log.seek(0)
        server_output = server_log.read()

    if server_process.returncode != 0:
        raise RuntimeError(f"server exited with code {server_process.returncode}\n{server_output}")

    accept_delta = end_sample["accept_total"] - warmup_sample["accept_total"]
    requests_delta = end_sample["requests_total"] - warmup_sample["requests_total"]
    reject_delta = end_sample["reject_total"] - warmup_sample["reject_total"]
    errors_delta = end_sample["errors_total"] - warmup_sample["errors_total"]
    measured_seconds = max(0.001, (end_sample["ts"] - warmup_sample["ts"]) / 1000.0)
    reuse_ratio = None if accept_delta == 0 else requests_delta / accept_delta
    qps = requests_delta / measured_seconds
    rss_delta_kb = end_sample["rss_kb"] - warmup_sample["rss_kb"]
    cpu_pct = ((cpu_end_seconds - cpu_start_seconds) / max(cpu_wall_seconds, 0.001)) * 100.0

    return BenchmarkRunResult(
        wrk_command=wrk_command,
        wrk_stdout=wrk_stdout,
        wrk_stderr=wrk_stderr,
        metrics_log_path=metrics_log_path,
        warmup_sample_ts_ms=warmup_sample["ts"],
        end_sample_ts_ms=end_sample["ts"],
        accept_delta=accept_delta,
        requests_delta=requests_delta,
        reject_delta=reject_delta,
        errors_delta=errors_delta,
        reuse_ratio=reuse_ratio,
        qps=qps,
        rss_delta_kb=rss_delta_kb,
        cpu_pct=cpu_pct,
    )


def main() -> int:
    args = parse_args()
    result = run_benchmark_once(args)

    print("wrk stdout:")
    print(result.wrk_stdout.rstrip())
    if result.wrk_stderr.strip():
        print("wrk stderr:")
        print(result.wrk_stderr.rstrip())

    print("measurement summary:")
    print(f"  warmup_sample_ts_ms={result.warmup_sample_ts_ms}")
    print(f"  end_sample_ts_ms={result.end_sample_ts_ms}")
    print(f"  accept_delta={result.accept_delta}")
    print(f"  requests_delta={result.requests_delta}")
    print(f"  reject_delta={result.reject_delta}")
    print(f"  errors_delta={result.errors_delta}")
    print(f"  qps={result.qps:.4f}")
    print(f"  rss_delta_kb={result.rss_delta_kb}")
    print(f"  cpu_pct={result.cpu_pct:.4f}")

    if result.accept_delta == 0 or result.reuse_ratio is None:
        raise RuntimeError(
            "benchmark run is invalid because accept_delta == 0 in the measurement window"
        )

    print(f"  reuse_ratio={result.reuse_ratio:.4f}")
    print(f"  reuse_threshold={args.reuse_threshold:.4f}")

    if result.reuse_ratio <= args.reuse_threshold:
        raise RuntimeError(
            f"reuse ratio check failed: {result.reuse_ratio:.4f} <= {args.reuse_threshold:.4f}"
        )

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"fatal: {exc}", file=sys.stderr)
        raise SystemExit(1)
