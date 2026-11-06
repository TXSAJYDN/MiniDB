#!/usr/bin/env python3
"""Collect reproducible Stage G RDB memory and SAVE-barrier evidence.

The workload builds a v2 RDB image at the exact 64 MiB format limit, measures
the server's peak RSS while recovering that image, then measures the peak RSS
and end-to-end latency of an online HTTP SAVE.  The evidence directory must be
new or empty; this tool never deletes an existing run.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import signal
import socket
import subprocess
import sys
import threading
import time


RDB_MAX_FILE_SIZE = 64 * 1024 * 1024
PROCESS_RSS_BUDGET_MIB = 512
SAVE_PAUSE_LIMIT_MS = 30_000
FULL_VALUE_SIZE = 1024 * 1024
# 63 full-size values plus this final value make the serialized v2 image
# exactly 64 MiB with 64 one-byte keys:
#   12-byte header + 64 * (1 key-len + 1 key + 3 value-len + 8 expiry)
#   + 63 * 1 MiB + 1_047_728-byte final value + 4-byte checksum.
FINAL_VALUE_SIZE = 1_047_728
KEYS = list("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")
assert len(KEYS) == 64 and len(set(KEYS)) == 64
assert 12 + 64 * 13 + 63 * FULL_VALUE_SIZE + FINAL_VALUE_SIZE + 4 == RDB_MAX_FILE_SIZE


class AcceptanceError(RuntimeError):
    pass


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def free_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def value_for(index):
    size = FULL_VALUE_SIZE if index < 63 else FINAL_VALUE_SIZE
    return bytes([index & 0xFF]) * size


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def command_output(command):
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False)
    return result.stdout.strip()


def read_proc_status(pid):
    status = {}
    try:
        lines = Path(f"/proc/{pid}/status").read_text(
            encoding="utf-8").splitlines()
    except (FileNotFoundError, ProcessLookupError):
        return None
    for line in lines:
        name, separator, value = line.partition(":")
        if separator and name in {"VmRSS", "VmHWM", "Threads"}:
            fields = value.split()
            if fields:
                status[name] = int(fields[0])
    if "VmRSS" not in status or "VmHWM" not in status:
        return None
    status["Threads"] = status.get("Threads", 0)
    return status


class RssSampler(threading.Thread):
    def __init__(self, pid):
        super().__init__(name="minidb-rss-sampler", daemon=True)
        self.pid = pid
        self.started = time.monotonic()
        self.stop_event = threading.Event()
        self.samples = []

    def run(self):
        while not self.stop_event.is_set():
            status = read_proc_status(self.pid)
            if status is None:
                break
            sample = {
                "elapsed_ms": round(
                    (time.monotonic() - self.started) * 1000.0, 3),
                "rss_kib": status["VmRSS"],
                "hwm_kib": status["VmHWM"],
                "threads": status["Threads"],
            }
            if not self.samples or any(
                    sample[key] != self.samples[-1][key]
                    for key in ("rss_kib", "hwm_kib", "threads")):
                self.samples.append(sample)
            self.stop_event.wait(0.005)

    def stop(self):
        self.stop_event.set()
        self.join(timeout=2)


def write_json_lines(path, samples):
    with path.open("w", encoding="utf-8") as output:
        for sample in samples:
            output.write(json.dumps(sample, sort_keys=True) + "\n")


class ServerProcess:
    def __init__(self, binary, port, rdb_path, log_path):
        self.binary = binary
        self.port = port
        self.rdb_path = rdb_path
        self.log_path = log_path
        self.log_file = log_path.open("wb")
        self.proc = subprocess.Popen(
            [str(binary), "-p", str(port), "-r", str(rdb_path)],
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

    def wait_ready(self, timeout=60.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise AcceptanceError(
                    f"server exited during startup with {self.proc.returncode}; "
                    f"log: {self.log_path}")
            try:
                status, body = http_once(self.port, "GET", "/health", timeout=1.0)
                if status == 200 and body == b"OK\n":
                    return
            except (OSError, AcceptanceError):
                pass
            time.sleep(0.02)
        raise AcceptanceError(
            f"server did not become healthy within {timeout}s; log: {self.log_path}")

    def stop(self, timeout=60.0):
        if self.proc.poll() is None:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=5)
            self.log_file.flush()
            self.log_file.close()
            raise AcceptanceError(
                f"server did not stop within {timeout}s; log: {self.log_path}")
        self.log_file.flush()
        self.log_file.close()
        return self.proc.returncode

    def log_tail(self, lines=30):
        text = self.log_path.read_text(encoding="utf-8", errors="replace")
        return "\n".join(text.splitlines()[-lines:])


class HttpClient:
    def __init__(self, port, timeout=120.0):
        self.sock = socket.create_connection(("127.0.0.1", port),
                                             timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.stream = self.sock.makefile("rb")

    def close(self):
        self.stream.close()
        self.sock.close()

    def request(self, method, path, body=b""):
        header = (
            f"{method} {path} HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: keep-alive\r\n"
        ).encode("ascii")
        if method == "POST" or body:
            header += f"Content-Length: {len(body)}\r\n".encode("ascii")
        self.sock.sendall(header + b"\r\n" + body)

        status_line = self.stream.readline()
        if not status_line.endswith(b"\r\n"):
            raise AcceptanceError("HTTP response ended before status line")
        fields = status_line[:-2].split(b" ", 2)
        if len(fields) < 2 or not fields[0].startswith(b"HTTP/"):
            raise AcceptanceError(f"malformed HTTP status line: {status_line!r}")
        status = int(fields[1])
        content_length = None
        while True:
            line = self.stream.readline()
            if not line.endswith(b"\r\n"):
                raise AcceptanceError("HTTP response ended in headers")
            line = line[:-2]
            if not line:
                break
            name, separator, value = line.partition(b":")
            if separator and name.strip().lower() == b"content-length":
                content_length = int(value.strip())
        if content_length is None:
            raise AcceptanceError("HTTP response omitted Content-Length")
        chunks = []
        remaining = content_length
        while remaining:
            chunk = self.stream.read(min(remaining, 1024 * 1024))
            if not chunk:
                raise AcceptanceError("HTTP response body was truncated")
            chunks.append(chunk)
            remaining -= len(chunk)
        return status, b"".join(chunks)


def http_once(port, method, path, body=b"", timeout=10.0):
    client = HttpClient(port, timeout=timeout)
    try:
        return client.request(method, path, body)
    finally:
        client.close()


def load_dataset(port):
    client = HttpClient(port)
    try:
        for index, key in enumerate(KEYS):
            body = value_for(index)
            status, response = client.request("POST", f"/kv/{key}", body)
            if status != 200 or response != b"OK\n":
                raise AcceptanceError(
                    f"dataset POST failed for key {key!r}: "
                    f"status={status}, body={response[:80]!r}")
    finally:
        client.close()


def verify_recovered_values(port):
    client = HttpClient(port)
    try:
        for index in (0, 63):
            status, body = client.request("GET", f"/kv/{KEYS[index]}")
            expected = value_for(index)
            if status != 200 or body != expected:
                raise AcceptanceError(
                    f"recovered key {KEYS[index]!r} mismatch: "
                    f"status={status}, bytes={len(body)}, expected={len(expected)}")
    finally:
        client.close()


def parse_save_response(status, body):
    if status != 200:
        raise AcceptanceError(f"online SAVE failed: status={status}, body={body!r}")
    try:
        payload = json.loads(body)
    except json.JSONDecodeError as error:
        raise AcceptanceError(f"SAVE returned invalid JSON: {body!r}") from error
    if payload.get("status") != "ok" or payload.get("keys") != len(KEYS):
        raise AcceptanceError(f"unexpected SAVE response: {payload!r}")
    return payload


def temporary_files(rdb_path):
    return sorted(str(path) for path in rdb_path.parent.glob(
        rdb_path.name + ".tmp.*"))


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default=".")
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--max-rss-mib", type=int,
                        default=PROCESS_RSS_BUDGET_MIB)
    parser.add_argument("--max-save-pause-ms", type=int,
                        default=SAVE_PAUSE_LIMIT_MS)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    source = Path(args.source).resolve()
    build_dir = Path(args.build_dir).resolve()
    evidence = Path(args.evidence).resolve()
    if evidence.exists() and any(evidence.iterdir()):
        raise AcceptanceError(f"evidence directory is not empty: {evidence}")
    evidence.mkdir(parents=True, exist_ok=True)
    logs = evidence / "logs"
    work = evidence / "work"
    logs.mkdir()
    work.mkdir()

    server_binary = build_dir / "app" / "minidb_http_server"
    if not server_binary.is_file():
        raise AcceptanceError(f"HTTP server binary is missing: {server_binary}")
    cache_path = build_dir / "CMakeCache.txt"
    cache = cache_path.read_text(encoding="utf-8", errors="replace")         if cache_path.is_file() else ""
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        raise AcceptanceError(
            "Stage G footprint evidence requires a Release build directory")
    for sanitizer in ("ENABLE_ASAN", "ENABLE_UBSAN", "ENABLE_TSAN"):
        if f"{sanitizer}:BOOL=ON" in cache:
            raise AcceptanceError(
                f"Stage G footprint evidence must use a non-sanitized build ({sanitizer}=ON)")

    rdb_path = work / "stage-g-max.rdb"
    generation_log = logs / "generation-server.log"
    recovery_log = logs / "recovery-server.log"

    generation_port = free_loopback_port()
    generation_server = ServerProcess(
        server_binary, generation_port, rdb_path, generation_log)
    generation_exit = None
    try:
        generation_server.wait_ready()
        load_dataset(generation_port)

        online_sampler = RssSampler(generation_server.proc.pid)
        online_sampler.start()
        time.sleep(0.02)
        before_stats = read_proc_status(generation_server.proc.pid)
        if before_stats is None:
            raise AcceptanceError("could not read online-save baseline RSS")

        client = HttpClient(generation_port)
        try:
            started_ns = time.perf_counter_ns()
            status, response = client.request("POST", "/save")
            save_elapsed_ms = (time.perf_counter_ns() - started_ns) / 1_000_000.0
        finally:
            client.close()
        save_payload = parse_save_response(status, response)
        time.sleep(0.05)
        online_sampler.stop()
        after_stats = read_proc_status(generation_server.proc.pid)
        if after_stats is None:
            raise AcceptanceError("could not read online-save peak RSS")
        write_json_lines(evidence / "rss-online-save.jsonl",
                         online_sampler.samples)
    finally:
        generation_exit = generation_server.stop()

    if generation_exit != 0:
        raise AcceptanceError(
            f"generation server exited {generation_exit}:\n"
            f"{generation_server.log_tail()}")

    if not rdb_path.is_file():
        raise AcceptanceError(f"RDB file was not created: {rdb_path}")
    rdb_size = rdb_path.stat().st_size
    if rdb_size != RDB_MAX_FILE_SIZE:
        raise AcceptanceError(
            f"generated RDB is {rdb_size} bytes, expected exact limit "
            f"{RDB_MAX_FILE_SIZE}")
    leftovers = temporary_files(rdb_path)
    if leftovers:
        raise AcceptanceError(f"temporary RDB files were left behind: {leftovers}")
    rdb_sha256 = sha256_file(rdb_path)

    recovery_port = free_loopback_port()
    recovery_server = ServerProcess(
        server_binary, recovery_port, rdb_path, recovery_log)
    recovery_exit = None
    try:
        startup_sampler = RssSampler(recovery_server.proc.pid)
        startup_sampler.start()
        recovery_server.wait_ready()
        # VmHWM is a kernel-maintained high-water mark, so reading after the
        # listener is ready captures the complete pre-listen recovery phase.
        time.sleep(0.1)
        startup_sampler.stop()
        startup_stats = read_proc_status(recovery_server.proc.pid)
        if startup_stats is None:
            raise AcceptanceError("could not read recovery peak RSS")
        write_json_lines(evidence / "rss-startup-recovery.jsonl",
                         startup_sampler.samples)
        verify_recovered_values(recovery_port)
    finally:
        recovery_exit = recovery_server.stop()

    if recovery_exit != 0:
        raise AcceptanceError(
            f"recovery server exited {recovery_exit}:\n"
            f"{recovery_server.log_tail()}")
    recovery_log_text = recovery_log.read_text(
        encoding="utf-8", errors="replace")
    if f"Loaded {len(KEYS)} keys from RDB file" not in recovery_log_text:
        raise AcceptanceError(
            "recovery log does not confirm all 64 keys were loaded")

    max_rss_kib = args.max_rss_mib * 1024
    online_peak_kib = after_stats["VmHWM"]
    startup_peak_kib = startup_stats["VmHWM"]
    summary = {
        "timestamp_utc": utc_now(),
        "source": str(source),
        "build_dir": str(build_dir),
        "server_binary": str(server_binary),
        "server_binary_sha256": sha256_file(server_binary),
        "git_head": command_output(
            ["git", "-C", str(source), "rev-parse", "HEAD"]),
        "git_status": command_output(
            ["git", "-C", str(source), "status", "--short"]),
        "platform": platform.platform(),
        "uname": command_output(["uname", "-a"]),
        "python": sys.version.splitlines()[0],
        "build_type": "Release",
        "rdb": {
            "path": str(rdb_path),
            "size_bytes": rdb_size,
            "size_mib": round(rdb_size / (1024 * 1024), 6),
            "size_limit_bytes": RDB_MAX_FILE_SIZE,
            "sha256": rdb_sha256,
            "record_count": len(KEYS),
            "final_value_bytes": FINAL_VALUE_SIZE,
            "temporary_files_after_save": leftovers,
        },
        "online_save": {
            "http_status": status,
            "response": save_payload,
            "request_elapsed_ms": round(save_elapsed_ms, 3),
            "pause_limit_ms": args.max_save_pause_ms,
            "rss_before_kib": before_stats["VmRSS"],
            "hwm_before_kib": before_stats["VmHWM"],
            "peak_rss_kib": online_peak_kib,
            "peak_rss_mib": round(online_peak_kib / 1024.0, 3),
            "samples": str(evidence / "rss-online-save.jsonl"),
            "server_log": str(generation_log),
            "server_exit_code": generation_exit,
        },
        "startup_recovery": {
            "peak_rss_kib": startup_peak_kib,
            "peak_rss_mib": round(startup_peak_kib / 1024.0, 3),
            "samples": str(evidence / "rss-startup-recovery.jsonl"),
            "server_log": str(recovery_log),
            "server_exit_code": recovery_exit,
            "recovered_keys": len(KEYS),
        },
        "budgets": {
            "process_rss_budget_mib": args.max_rss_mib,
            "process_rss_budget_kib": max_rss_kib,
            "save_pause_limit_ms": args.max_save_pause_ms,
        },
        "command": shlex.join([sys.executable, *sys.argv]),
        "assertions": {},
    }

    summary["assertions"] = {
        "rdb_is_exact_format_limit": rdb_size == RDB_MAX_FILE_SIZE,
        "save_http_ok_and_64_keys": (
            status == 200 and save_payload.get("keys") == len(KEYS)),
        "save_pause_within_limit": save_elapsed_ms <= args.max_save_pause_ms,
        "startup_peak_within_budget": startup_peak_kib <= max_rss_kib,
        "online_save_peak_within_budget": online_peak_kib <= max_rss_kib,
        "no_temporary_rdb_leftovers": not leftovers,
        "recovery_log_confirms_64_keys": True,
    }
    failed = [name for name, value in summary["assertions"].items() if not value]
    summary["result"] = "PASS" if not failed else "FAIL"
    summary["failed_assertions"] = failed
    (evidence / "stage-g-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    if failed:
        raise AcceptanceError(f"Stage G assertions failed: {', '.join(failed)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AcceptanceError, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
