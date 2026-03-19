#!/usr/bin/env python3

import argparse
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


RESPONSE_BODY = b"RouteC MVP response\n"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the Week3 Day4 sanitizer smoke workload against the webserver."
    )
    parser.add_argument("--server-path", required=True, help="Path to the webserver binary")
    parser.add_argument("--host", default="127.0.0.1", help="Server bind host")
    parser.add_argument("--port", type=int, default=18080, help="Server bind port")
    parser.add_argument(
        "--duration-sec",
        type=float,
        default=3.0,
        help="How long to keep issuing the core workload",
    )
    return parser.parse_args()


def read_http_response(sock: socket.socket) -> tuple[int, bytes]:
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("socket closed before HTTP header completed")
        data.extend(chunk)

    header_bytes, body = data.split(b"\r\n\r\n", 1)
    header_lines = header_bytes.decode("latin1").split("\r\n")
    status_parts = header_lines[0].split(" ", 2)
    if len(status_parts) < 2:
        raise RuntimeError(f"invalid status line: {header_lines[0]!r}")

    headers: dict[str, str] = {}
    for line in header_lines[1:]:
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        headers[key.strip().lower()] = value.strip()

    content_length = int(headers.get("content-length", "0"))
    while len(body) < content_length:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("socket closed before HTTP body completed")
        body += chunk

    return int(status_parts[1]), bytes(body[:content_length])


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


def read_latest_metrics(log_path: pathlib.Path) -> dict[str, int]:
    lines = [line.strip() for line in log_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if not lines:
        return {}

    metrics: dict[str, int] = {}
    for token in lines[-1].split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        metrics[key] = int(value)
    return metrics


def wait_for_metrics(log_path: pathlib.Path, expected: dict[str, int], timeout_sec: float) -> dict[str, int]:
    deadline = time.monotonic() + timeout_sec
    latest: dict[str, int] = {}
    while time.monotonic() < deadline:
        latest = read_latest_metrics(log_path)
        if (
            latest
            and all(latest.get(key, -1) >= value for key, value in expected.items())
            and latest.get("conns_current") == 0
            and latest.get("global_inflight_bytes_current") == 0
        ):
            return latest
        time.sleep(0.1)

    raise RuntimeError(f"metrics did not reach expected values: latest={latest}, expected={expected}")


def keepalive_get_worker(
    host: str,
    port: int,
    request_count: int,
    counters: dict[str, int],
    lock: threading.Lock,
) -> None:
    request = (
        b"GET / HTTP/1.1\r\n"
        b"Host: 127.0.0.1\r\n"
        b"Connection: keep-alive\r\n"
        b"\r\n"
    )

    with socket.create_connection((host, port), timeout=1.0) as sock:
        sock.settimeout(2.0)
        with lock:
            counters["accept_total"] += 1

        for _ in range(request_count):
            sock.sendall(request)
            status_code, body = read_http_response(sock)
            if status_code != 200 or body != RESPONSE_BODY:
                raise RuntimeError(f"unexpected GET response: status={status_code}, body={body!r}")
            with lock:
                counters["requests_total"] += 1


def keepalive_post_worker(
    host: str,
    port: int,
    request_count: int,
    counters: dict[str, int],
    lock: threading.Lock,
) -> None:
    body = b"hello"
    request = (
        b"POST / HTTP/1.1\r\n"
        b"Host: 127.0.0.1\r\n"
        b"Connection: keep-alive\r\n"
        b"Content-Length: 5\r\n"
        b"\r\n" + body
    )

    with socket.create_connection((host, port), timeout=1.0) as sock:
        sock.settimeout(2.0)
        with lock:
            counters["accept_total"] += 1

        for _ in range(request_count):
            sock.sendall(request)
            status_code, response_body = read_http_response(sock)
            if status_code != 200 or response_body != RESPONSE_BODY:
                raise RuntimeError(
                    f"unexpected POST response: status={status_code}, body={response_body!r}"
                )
            with lock:
                counters["requests_total"] += 1


def reject_worker(
    host: str,
    port: int,
    request_count: int,
    counters: dict[str, int],
    lock: threading.Lock,
) -> None:
    request = (
        b"POST / HTTP/1.1\r\n"
        b"Host: 127.0.0.1\r\n"
        b"\r\n"
    )

    for _ in range(request_count):
        with socket.create_connection((host, port), timeout=1.0) as sock:
            sock.settimeout(2.0)
            with lock:
                counters["accept_total"] += 1

            sock.sendall(request)
            status_code, body = read_http_response(sock)
            if status_code != 411 or body != b"Length Required\n":
                raise RuntimeError(f"unexpected reject response: status={status_code}, body={body!r}")
            with lock:
                counters["reject_total"] += 1
                counters["reject_411_total"] += 1
                counters["errors_total"] += 1
            time.sleep(0.1)


def main() -> int:
    args = parse_args()
    project_root = pathlib.Path(__file__).resolve().parent.parent
    server_path = pathlib.Path(args.server_path).resolve()
    if not server_path.exists():
        raise RuntimeError(f"server binary does not exist: {server_path}")

    counters = {
        "accept_total": 0,
        "requests_total": 0,
        "reject_total": 0,
        "reject_411_total": 0,
        "errors_total": 0,
    }
    lock = threading.Lock()
    worker_errors: list[BaseException] = []

    # [Week3 Day4] Begin:
    # Day4 的目标不是做 wrk，而是在 sanitizer 口径下持续重放当前已经存在的核心路径：
    # 1. keep-alive GET
    # 2. 带 body 的 POST
    # 3. 缺 Content-Length 的 411 拒绝
    # 这样可以把“正常请求 + body 状态机 + 错误分支 + metrics 计数”一起压到同一次自测里。
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
            get_request_count = max(8, int(args.duration_sec * 20))
            post_request_count = max(4, int(args.duration_sec * 10))
            reject_request_count = max(2, int(args.duration_sec * 3))

            def run_worker(worker_fn) -> None:
                try:
                    if worker_fn is keepalive_get_worker:
                        worker_fn(args.host, args.port, get_request_count, counters, lock)
                    elif worker_fn is keepalive_post_worker:
                        worker_fn(args.host, args.port, post_request_count, counters, lock)
                    else:
                        worker_fn(args.host, args.port, reject_request_count, counters, lock)
                except BaseException as exc:  # noqa: BLE001
                    worker_errors.append(exc)

            workers = [
                threading.Thread(
                    target=run_worker,
                    args=(keepalive_get_worker,),
                    daemon=True,
                ),
                threading.Thread(
                    target=run_worker,
                    args=(keepalive_post_worker,),
                    daemon=True,
                ),
                threading.Thread(
                    target=run_worker,
                    args=(reject_worker,),
                    daemon=True,
                ),
            ]

            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join()

            if worker_errors:
                raise RuntimeError(f"worker failed: {worker_errors[0]}")

            metrics_log = find_latest_metrics_log(project_root)
            expected_metrics = dict(counters)
            wait_for_metrics(metrics_log, expected_metrics, timeout_sec=5.0)
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
    # [Week3 Day4] End

    if server_process.returncode != 0:
        raise RuntimeError(f"server exited with code {server_process.returncode}\n{server_output}")

    sanitizer_markers = (
        "ERROR: AddressSanitizer",
        "runtime error:",
        "UndefinedBehaviorSanitizer",
    )
    if any(marker in server_output for marker in sanitizer_markers):
        raise RuntimeError(f"sanitizer reported an error\n{server_output}")

    print("sanitizer smoke passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"fatal: {exc}", file=sys.stderr)
        raise SystemExit(1)
