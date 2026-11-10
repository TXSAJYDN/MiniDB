#!/usr/bin/env python3
"""Reproducible single-connection HTTP and KV read benchmarks for MiniDB.

The benchmark intentionally uses the Python standard library only.  It starts
each server on an ephemeral loopback port with a private temporary RDB, loads a
fixed dataset, performs an untimed warm-up, then records every request latency.
Host-level percentiles include client and scheduler overhead; interpret the
results as a WSL2 workstation baseline, not bare-metal capacity.
"""

import argparse
import datetime
import json
import os
import platform
from pathlib import Path
import signal
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time


KV_SET = 2
KV_GET = 3
KV_OK = 101
KV_VALUE = 102


def free_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def percentile(sorted_samples, fraction):
    if not sorted_samples:
        return 0.0
    position = fraction * (len(sorted_samples) - 1)
    low = int(position)
    high = min(low + 1, len(sorted_samples) - 1)
    weight = position - low
    return sorted_samples[low] * (1.0 - weight) + sorted_samples[high] * weight


def latency_summary(samples_us):
    ordered = sorted(samples_us)
    return {
        "min_us": round(ordered[0], 3),
        "mean_us": round(statistics.fmean(ordered), 3),
        "p50_us": round(percentile(ordered, 0.50), 3),
        "p95_us": round(percentile(ordered, 0.95), 3),
        "p99_us": round(percentile(ordered, 0.99), 3),
        "p999_us": round(percentile(ordered, 0.999), 3),
        "max_us": round(ordered[-1], 3),
    }


class ServerProcess:
    def __init__(self, executable, port, rdb_path, log_path,
                 durability_mode=None):
        self.port = port
        self.log_file = open(log_path, "wb")
        command = [executable, "-p", str(port), "-r", rdb_path]
        if durability_mode is not None:
            command += ["-d", durability_mode]
        self.proc = subprocess.Popen(
            command,
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

    def wait_ready(self, protocol):
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"server exited with {self.proc.returncode} during startup")
            try:
                with socket.create_connection(("127.0.0.1", self.port),
                                              timeout=0.25) as sock:
                    if protocol == "kv":
                        sock.sendall(kv_encode(1, []))
                        if kv_read_frame(sock)[0] == KV_OK:
                            return
                    else:
                        sock.sendall(
                            b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            b"Connection: close\r\n\r\n")
                        if b" 200 " in sock.recv(4096):
                            return
            except (ConnectionRefusedError, OSError, ValueError):
                pass
            time.sleep(0.02)
        raise RuntimeError("server did not become ready within 10 seconds")

    def stop(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=2.0)
        self.log_file.close()
        return self.proc.returncode


def kv_encode(command_type, args):
    payload = bytes((command_type, len(args)))
    for arg in args:
        raw = arg.encode("ascii") if isinstance(arg, str) else arg
        payload += struct.pack("!I", len(raw)) + raw
    return struct.pack("!I", len(payload)) + payload


def recv_exact(stream, count):
    receiver = getattr(stream, "recv", None)
    if receiver is None:
        receiver = stream.read
    chunks = []
    remaining = count
    while remaining:
        chunk = receiver(remaining)
        if not chunk:
            raise RuntimeError("unexpected EOF from server")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def kv_read_frame(sock):
    frame_length = struct.unpack("!I", recv_exact(sock, 4))[0]
    payload = recv_exact(sock, frame_length)
    if len(payload) < 2:
        raise RuntimeError("short KV response")
    command_type = payload[0]
    argc = payload[1]
    offset = 2
    args = []
    for _ in range(argc):
        if frame_length - offset < 4:
            raise RuntimeError("truncated KV argument length")
        arg_length = struct.unpack("!I", payload[offset:offset + 4])[0]
        offset += 4
        if arg_length > frame_length - offset:
            raise RuntimeError("truncated KV argument")
        args.append(payload[offset:offset + arg_length])
        offset += arg_length
    if offset != frame_length:
        raise RuntimeError("trailing KV response bytes")
    return command_type, args


def read_http_line(stream):
    line = stream.readline()
    if not line.endswith(b"\r\n"):
        raise RuntimeError("unexpected EOF in HTTP response")
    return line[:-2]


def http_request(sock, stream, method, path, body=b""):
    request = (
        f"{method} {path} HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: keep-alive\r\n"
    ).encode("ascii")
    if method == "POST" or body:
        request += f"Content-Length: {len(body)}\r\n".encode("ascii")
    request += b"\r\n" + body
    sock.sendall(request)

    status_line = read_http_line(stream)
    parts = status_line.split(b" ", 2)
    if len(parts) < 2 or not parts[0].startswith(b"HTTP/"):
        raise RuntimeError(f"malformed HTTP status line: {status_line!r}")
    status = int(parts[1])
    content_length = None
    while True:
        line = read_http_line(stream)
        if not line:
            break
        name, separator, value = line.partition(b":")
        if separator and name.strip().lower() == b"content-length":
            content_length = int(value.strip())
    if content_length is None:
        raise RuntimeError("HTTP response omitted Content-Length")
    response_body = recv_exact(stream, content_length) if content_length else b""
    return status, response_body


def make_dataset(key_count, value_size):
    return [
        (f"bench-key-{i:04d}",
         bytes(((i * 31 + j * 17) & 0xff for j in range(value_size))))
        for i in range(key_count)
    ]


def load_http_dataset(port, dataset):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    stream = sock.makefile("rb")
    try:
        for key, value in dataset:
            status, body = http_request(
                sock, stream, "POST", f"/kv/{key}", value)
            if status != 200 or body != b"OK\n":
                raise RuntimeError(
                    f"HTTP dataset load failed for {key}: {status} {body!r}")
    finally:
        stream.close()
        sock.close()


def load_kv_dataset(port, dataset):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        for key, value in dataset:
            sock.sendall(kv_encode(KV_SET, [key, value]))
            command_type, _ = kv_read_frame(sock)
            if command_type != KV_OK:
                raise RuntimeError(
                    f"KV dataset load failed for {key}: {command_type}")
    finally:
        sock.close()


def benchmark_http(port, dataset, warmup, samples):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    stream = sock.makefile("rb")

    def one(index):
        key, expected = dataset[index % len(dataset)]
        status, body = http_request(sock, stream, "GET", f"/kv/{key}")
        if status != 200 or body != expected:
            raise RuntimeError(
                f"HTTP GET mismatch for {key}: status={status}, bytes={len(body)}")

    try:
        for index in range(warmup):
            one(index)
        latencies = []
        started = time.perf_counter()
        for index in range(samples):
            begin = time.perf_counter_ns()
            one(index)
            latencies.append((time.perf_counter_ns() - begin) / 1000.0)
        elapsed = time.perf_counter() - started
    finally:
        stream.close()
        sock.close()
    return latencies, elapsed


def benchmark_kv(port, dataset, warmup, samples):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def one(index):
        key, expected = dataset[index % len(dataset)]
        sock.sendall(kv_encode(KV_GET, [key]))
        command_type, args = kv_read_frame(sock)
        if command_type != KV_VALUE or len(args) != 1 or args[0] != expected:
            raise RuntimeError(
                f"KV GET mismatch for {key}: command={command_type}")

    try:
        for index in range(warmup):
            one(index)
        latencies = []
        started = time.perf_counter()
        for index in range(samples):
            begin = time.perf_counter_ns()
            one(index)
            latencies.append((time.perf_counter_ns() - begin) / 1000.0)
        elapsed = time.perf_counter() - started
    finally:
        sock.close()
    return latencies, elapsed


def environment_record():
    load_average = os.getloadavg() if hasattr(os, "getloadavg") else (0, 0, 0)
    return {
        "platform": platform.platform(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "cpu_count": os.cpu_count(),
        "load_average": [round(value, 3) for value in load_average],
        "wsl2": "microsoft" in platform.release().lower(),
    }


def run_protocol(protocol, executable, dataset, warmup, samples, temp_dir):
    port = free_loopback_port()
    rdb_path = os.path.join(temp_dir, f"{protocol}.rdb")
    log_path = os.path.join(temp_dir, f"{protocol}-server.log")
    server = ServerProcess(executable, port, rdb_path, log_path)
    server.port = port
    try:
        server.wait_ready(protocol)
        if protocol == "http":
            load_http_dataset(port, dataset)
            latencies, elapsed = benchmark_http(
                port, dataset, warmup, samples)
        else:
            load_kv_dataset(port, dataset)
            latencies, elapsed = benchmark_kv(
                port, dataset, warmup, samples)
        return_code = server.stop()
        if return_code != 0:
            raise RuntimeError(
                f"{protocol} server exited with {return_code}; log={log_path}")
    except Exception:
        server.stop()
        raise

    return {
        "protocol": protocol,
        "operation": "GET",
        "warmup_requests": warmup,
        "measured_requests": samples,
        "elapsed_seconds": round(elapsed, 6),
        "throughput_requests_per_second": round(samples / elapsed, 3),
        "latency": latency_summary(latencies),
        "raw_latency_us": [round(value, 3) for value in latencies],
        "server_rdb": os.path.basename(rdb_path),
        "server_log": os.path.basename(log_path),
    }


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--http", required=True,
                        help="path to minidb_http_server")
    parser.add_argument("--kv", required=True,
                        help="path to minidb_kv_server")
    parser.add_argument("--samples", type=int, default=2000)
    parser.add_argument("--warmup", type=int, default=200)
    parser.add_argument("--keys", type=int, default=256)
    parser.add_argument("--value-size", type=int, default=256)
    parser.add_argument("--out", default="bench/results/latest.json")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.samples <= 0 or args.warmup < 0 or args.keys <= 0:
        raise SystemExit("samples/keys must be positive and warmup non-negative")
    if args.value_size <= 0:
        raise SystemExit("value-size must be positive")

    dataset = make_dataset(args.keys, args.value_size)
    with tempfile.TemporaryDirectory(prefix="minidb-bench-") as temp_dir:
        http_result = run_protocol(
            "http", args.http, dataset, args.warmup, args.samples, temp_dir)
        kv_result = run_protocol(
            "kv", args.kv, dataset, args.warmup, args.samples, temp_dir)

    output = {
        "schema": "minidb-benchmark-v1",
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "environment": environment_record(),
        "configuration": {
            "dataset_keys": args.keys,
            "value_size_bytes": args.value_size,
            "warmup_requests_per_protocol": args.warmup,
            "measured_requests_per_protocol": args.samples,
            "connection_model": "one persistent loopback connection per protocol",
            "client": "Python standard library socket client",
        },
        "limitations": [
            "WSL2 virtualized networking and shared host scheduling affect latency.",
            "Client and server compete for the same host CPU.",
            "Results cover single-connection sequential GET throughput, not saturation or multi-client scaling.",
            "Percentiles include Python client and scheduler overhead.",
        ],
        "results": [http_result, kv_result],
    }
    destination = Path(args.out)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "out": str(destination),
        "results": [
            {
                "protocol": item["protocol"],
                "throughput_requests_per_second":
                    item["throughput_requests_per_second"],
                "latency": item["latency"],
            }
            for item in output["results"]
        ],
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
