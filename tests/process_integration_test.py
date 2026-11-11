#!/usr/bin/env python3
"""Process-level acceptance tests for the MiniDB HTTP and KV executables.

Each case uses an ephemeral loopback port and a private temporary directory.
Server processes are started in a new session so cleanup and watchdog expiry
can terminate the whole process group.  The script is intentionally limited to
the Python standard library and is registered with CTest.
"""

import argparse
import hashlib
import json
import os
import random
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path


HTTP_BIN = ""
KV_BIN = ""

HTTP_PING = 1
HTTP_SET = 2
HTTP_GET = 3
HTTP_EXPIRE = 5
HTTP_TTL = 6
KV_DEL = 4
KV_SAVE = 7
KV_STATS = 8


class TestError(Exception):
    pass


def check(condition, message):
    if not condition:
        raise TestError(message)


def remove_persistence_files(rdb_path):
    for suffix in ("", ".manifest", ".lock"):
        try:
            os.unlink(rdb_path + suffix)
        except FileNotFoundError:
            pass
    shutil.rmtree(rdb_path + ".wal", ignore_errors=True)


def free_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def recv_exact(sock, count):
    chunks = []
    remaining = count
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise TestError("unexpected EOF while reading from server")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_http_response(port, method, path, body=b"", timeout=5.0):
    request = (
        f"{method} {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Connection: close\r\n"
    ).encode("ascii")
    if method == "POST" or body:
        request += f"Content-Length: {len(body)}\r\n".encode("ascii")
    request += b"\r\n" + body

    deadline = time.monotonic() + timeout
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(request)
        received = b""
        while b"\r\n\r\n" not in received:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TestError("timed out reading HTTP headers")
            sock.settimeout(remaining)
            chunk = sock.recv(65536)
            if not chunk:
                raise TestError("EOF before HTTP headers completed")
            received += chunk

        raw_headers, payload = received.split(b"\r\n\r\n", 1)
        lines = raw_headers.decode("iso-8859-1").split("\r\n")
        status_parts = lines[0].split(" ", 2)
        check(len(status_parts) >= 2 and status_parts[0].startswith("HTTP/"),
              f"malformed HTTP status line: {lines[0]!r}")
        status = int(status_parts[1])
        headers = {}
        for line in lines[1:]:
            name, separator, value = line.partition(":")
            check(bool(separator), f"malformed HTTP header line: {line!r}")
            headers.setdefault(name.strip().lower(), value.strip())

        content_length = int(headers.get("content-length", "0"))
        while len(payload) < content_length:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TestError("timed out reading HTTP body")
            sock.settimeout(remaining)
            chunk = sock.recv(content_length - len(payload))
            if not chunk:
                raise TestError("EOF before HTTP body completed")
            payload += chunk
        check(len(payload) == content_length,
              f"HTTP body length {len(payload)} != Content-Length {content_length}")
        return status, headers, payload


def kv_encode(command_type, args=()):
    payload = bytes((command_type, len(args)))
    for arg in args:
        raw = arg.encode("utf-8") if isinstance(arg, str) else arg
        payload += struct.pack("!I", len(raw)) + raw
    return struct.pack("!I", len(payload)) + payload


def kv_read_frame(sock):
    frame_length = struct.unpack("!I", recv_exact(sock, 4))[0]
    payload = recv_exact(sock, frame_length)
    check(len(payload) >= 2, "KV response shorter than command header")
    command_type = payload[0]
    arg_count = payload[1]
    offset = 2
    args = []
    for _ in range(arg_count):
        check(frame_length - offset >= 4, "truncated KV argument length")
        arg_length = struct.unpack("!I", payload[offset:offset + 4])[0]
        offset += 4
        check(arg_length <= frame_length - offset,
              "truncated KV argument payload")
        args.append(payload[offset:offset + arg_length])
        offset += arg_length
    check(offset == frame_length, "trailing bytes in KV response frame")
    return command_type, args


def kv_request(port, command_type, args=(), timeout=5.0):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(kv_encode(command_type, args))
        return kv_read_frame(sock)


class ServerProcess:
    def __init__(self, executable, port, rdb_path=None, cwd=None,
                 durability_mode=None):
        command = [executable, "-p", str(port)]
        if rdb_path is not None:
            command += ["-r", rdb_path]
        if durability_mode is not None:
            command += ["-d", durability_mode]
        self.proc = subprocess.Popen(
            command,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        self.log = ""

    def _read_log(self):
        if self.log:
            return self.log
        if self.proc.stdout is not None:
            try:
                self.log = self.proc.stdout.read() or ""
            except ValueError:
                self.log = ""
        return self.log

    def wait_for_http(self, port, timeout=10.0):
        deadline = time.monotonic() + timeout
        last_error = None
        while time.monotonic() < deadline:
            return_code = self.proc.poll()
            if return_code is not None:
                raise TestError(
                    f"HTTP server exited before readiness with {return_code}:\n"
                    f"{self._read_log()}")
            try:
                status, _, payload = read_http_response(
                    port, "GET", "/health", timeout=0.5)
                if status == 200 and payload == b"OK\n":
                    return
                last_error = TestError(f"health returned {status}: {payload!r}")
            except (OSError, TestError, ValueError) as error:
                last_error = error
            time.sleep(0.05)
        raise TestError(f"HTTP server did not become ready: {last_error}\n"
                        f"{self._read_log()}")

    def wait_for_kv(self, port, timeout=10.0):
        deadline = time.monotonic() + timeout
        last_error = None
        while time.monotonic() < deadline:
            return_code = self.proc.poll()
            if return_code is not None:
                raise TestError(
                    f"KV server exited before readiness with {return_code}:\n"
                    f"{self._read_log()}")
            try:
                command_type, args = kv_request(port, HTTP_PING, timeout=0.5)
                if command_type == 101 and args == [b"PONG"]:
                    return
                last_error = TestError(
                    f"PING returned type={command_type}, args={args!r}")
            except (OSError, TestError, ValueError) as error:
                last_error = error
            time.sleep(0.05)
        raise TestError(f"KV server did not become ready: {last_error}\n"
                        f"{self._read_log()}")

    def wait(self, timeout=10.0):
        try:
            return_code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.kill_group()
            raise TestError(
                f"server exceeded {timeout:.1f}s shutdown watchdog:\n"
                f"{self._read_log()}") from error
        self._read_log()
        return return_code

    def terminate_group(self, sig=signal.SIGTERM, timeout=10.0):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, sig)
        return self.wait(timeout=timeout)

    def kill_group(self):
        if self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            self.proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            pass
        self._read_log()

    def cleanup(self):
        self.kill_group()


def start_http(port, rdb_path, cwd, durability_mode=None):
    server = ServerProcess(HTTP_BIN, port, rdb_path, cwd, durability_mode)
    try:
        server.wait_for_http(port)
    except Exception:
        server.cleanup()
        raise
    return server


def start_kv(port, rdb_path, cwd, durability_mode=None):
    server = ServerProcess(KV_BIN, port, rdb_path, cwd, durability_mode)
    try:
        server.wait_for_kv(port)
    except Exception:
        server.cleanup()
        raise
    return server


def process_thread_count(pid):
    try:
        status = Path(f"/proc/{pid}/status").read_text(encoding="ascii")
    except FileNotFoundError as error:
        raise TestError(f"process {pid} exited while reading thread count") from error
    for line in status.splitlines():
        if line.startswith("Threads:"):
            return int(line.split(":", 1)[1].strip())
    raise TestError(f"process {pid} status omitted Threads")


def wait_for_thread_count(pid, minimum, timeout=3.0):
    deadline = time.monotonic() + timeout
    observed = 0
    while time.monotonic() < deadline:
        observed = process_thread_count(pid)
        if observed >= minimum:
            return observed
        time.sleep(0.02)
    raise TestError(
        f"process {pid} has {observed} threads, expected at least {minimum}")


def assert_listener_absent(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3):
            pass
    except ConnectionRefusedError:
        return
    except OSError:
        return
    raise TestError(f"port {port} accepted a connection but no listener was expected")


def run_cli(binary, args, cwd):
    return subprocess.run(
        [binary] + args,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=5.0,
    )


def wait_until(predicate, timeout, message):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, TestError, ValueError, json.JSONDecodeError) as error:
            last_error = error
        time.sleep(0.05)
    raise TestError(f"{message}; last_error={last_error!r}")


def read_http_stats(port):
    status, headers, payload = read_http_response(port, "GET", "/stats")
    check(status == 200,
          f"GET /stats returned {status}: {payload!r}")
    check(headers.get("content-type") == "application/json",
          f"GET /stats content type is {headers.get('content-type')!r}")
    return json.loads(payload.decode("utf-8"))


def read_kv_stats(port):
    command_type, args = kv_request(port, KV_STATS)
    check(command_type == 102 and len(args) == 1,
          f"KV STATS returned type={command_type}, args={args!r}")
    return json.loads(args[0].decode("utf-8"))


def stats_for(protocol, port):
    return read_http_stats(port) if protocol == "http" else read_kv_stats(port)


def check_timing_contract(stats, label):
    timing = stats.get("timing")
    check(isinstance(timing, dict), f"{label} stats omitted timing: {stats!r}")
    required = (
        "wal_append_total_us",
        "wal_append_max_us",
        "wal_data_fsync_total_us",
        "wal_data_fsync_max_us",
        "sync_mutation_total_us",
        "sync_mutation_max_us",
    )
    for name in required:
        check(isinstance(timing.get(name), int) and timing[name] >= 0,
              f"{label} timing field {name} is invalid: {stats!r}")
    check(timing["wal_append_total_us"] >= timing["wal_append_max_us"],
          f"{label} append timing is inconsistent: {stats!r}")
    check(timing["wal_data_fsync_total_us"] >=
          timing["wal_data_fsync_max_us"],
          f"{label} data-fsync timing is inconsistent: {stats!r}")
    check(timing["sync_mutation_total_us"] >=
          timing["sync_mutation_max_us"],
          f"{label} SYNC timing is inconsistent: {stats!r}")
    bounds = timing.get("fsync_latency_bucket_bounds_us")
    counts = timing.get("fsync_latency_bucket_counts")
    check(isinstance(bounds, list) and bounds and
          all(isinstance(value, int) and value > 0 for value in bounds) and
          bounds == sorted(bounds),
          f"{label} fsync bucket bounds are invalid: {stats!r}")
    check(isinstance(counts, list) and len(counts) == len(bounds) + 1 and
          all(isinstance(value, int) and value >= 0 for value in counts),
          f"{label} fsync bucket counts are invalid: {stats!r}")
    check(sum(counts) == stats["wal"]["data_fsyncs"],
          f"{label} fsync buckets do not partition data_fsyncs: {stats!r}")


def expect_startup_failure(binary, label, port, rdb_path, cwd, expected):
    result = subprocess.run(
        [binary, "-p", str(port), "-r", rdb_path],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=15.0,
        start_new_session=True,
    )
    check(result.returncode == 1,
          f"{label} accepted an invalid persistence image with exit "
          f"{result.returncode}:\n{result.stdout}")
    output = result.stdout.lower()
    check(expected in output,
          f"{label} failure omitted {expected!r}:\n{result.stdout}")
    assert_listener_absent(port)
    return result.stdout


def wal_segments(rdb_path):
    wal_dir = Path(rdb_path + ".wal")
    check(wal_dir.is_dir(), f"WAL directory is missing: {wal_dir}")
    segments = sorted(wal_dir.glob("wal-*.log"))
    check(bool(segments), f"no WAL segments in {wal_dir}")
    return segments


def sha256_path(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def test_cli_contract():
    with tempfile.TemporaryDirectory(prefix="minidb-process-cli-") as temp_dir:
        for binary, label in ((HTTP_BIN, "HTTP"), (KV_BIN, "KV")):
            help_run = run_cli(binary, ["-h"], temp_dir)
            check(help_run.returncode == 0,
                  f"{label} -h returned {help_run.returncode}: {help_run.stdout}")
            check("Usage:" in help_run.stdout,
                  f"{label} -h omitted usage text: {help_run.stdout}")
            check("-d" in help_run.stdout and "interval" in help_run.stdout,
                  f"{label} -h omitted durability modes: {help_run.stdout}")

            invalid_cases = (
                ["-p", "0"],
                ["-p", "65536"],
                ["-p", "not-a-port"],
                ["-p", "1", "-p", "2"],
                ["-r"],
                ["-d"],
                ["-d", "batch"],
                ["-d", "SYNC"],
                ["-d", "sync", "-d", "interval"],
                ["unexpected"],
            )
            for args in invalid_cases:
                result = run_cli(binary, args, temp_dir)
                check(result.returncode == 2,
                      f"{label} {' '.join(args)} returned {result.returncode}, "
                      f"expected 2:\n{result.stdout}")
        check(not any(name.endswith(".rdb") for name in os.listdir(temp_dir)),
              "CLI-only invocations unexpectedly created an RDB file")

        # Both executables must have a usable persistence default when -r is
        # omitted.  This catches a regression where the KV app passed an empty
        # path to RdbManager and only failed during shutdown.
        for binary, label, wait_name in (
                (HTTP_BIN, "HTTP", "http"),
                (KV_BIN, "KV", "kv")):
            port = free_loopback_port()
            default_rdb = os.path.join(temp_dir, "minidb.rdb")
            remove_persistence_files(default_rdb)
            server = ServerProcess(binary, port, cwd=temp_dir)
            try:
                if wait_name == "http":
                    server.wait_for_http(port)
                else:
                    server.wait_for_kv(port)
                return_code = server.terminate_group(signal.SIGTERM)
                check(return_code == 0,
                      f"{label} default-RDB shutdown returned {return_code}:\n"
                      f"{server.log}")
                check(os.path.isfile(default_rdb) and os.path.getsize(default_rdb) >= 16,
                      f"{label} did not create a valid default RDB")
            finally:
                server.cleanup()
            remove_persistence_files(default_rdb)


def test_cli_durability_contract():
    with tempfile.TemporaryDirectory(prefix="minidb-process-mode-") as temp_dir:
        for binary, label, protocol in (
                (HTTP_BIN, "HTTP", "http"),
                (KV_BIN, "KV", "kv")):
            rdb_path = os.path.join(temp_dir, f"{protocol}-interval.rdb")
            port = free_loopback_port()
            server = ServerProcess(
                binary, port, rdb_path, temp_dir, durability_mode="interval")
            try:
                if protocol == "http":
                    server.wait_for_http(port)
                else:
                    server.wait_for_kv(port)
                stats = stats_for(protocol, port)
                check(stats["durability_mode"] == "interval",
                      f"{label} did not expose interval mode: {stats!r}")
                check(stats["opened"] is True and stats["failed"] is False,
                      f"{label} reported an unhealthy open state: {stats!r}")
                check(server.terminate_group(signal.SIGTERM) == 0,
                      f"{label} interval shutdown failed:\n{server.log}")
            finally:
                server.cleanup()


def test_http_persistence_and_sigterm():
    with tempfile.TemporaryDirectory(prefix="minidb-process-http-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "http.rdb")
        port = free_loopback_port()
        server = start_http(port, rdb_path, temp_dir)
        try:
            value = b"persisted\x00http\xffvalue"
            status, _, payload = read_http_response(
                port, "POST", "/kv/process-key", value)
            check(status == 200 and payload == b"OK\n",
                  f"HTTP POST failed: status={status}, body={payload!r}")
            status, _, payload = read_http_response(
                port, "GET", "/kv/process-key")
            check(status == 200 and payload == value,
                  f"HTTP GET mismatch: status={status}, body={payload!r}")

            return_code = server.terminate_group(signal.SIGTERM)
            check(return_code == 0,
                  f"HTTP SIGTERM shutdown returned {return_code}:\n{server.log}")
            check("checkpoint saved" in server.log,
                  f"HTTP shutdown did not report a saved RDB:\n{server.log}")
        finally:
            server.cleanup()

        check(os.path.isfile(rdb_path) and os.path.getsize(rdb_path) > 0,
              "HTTP SIGTERM did not create a non-empty RDB snapshot")

        restarted = start_http(port, rdb_path, temp_dir)
        try:
            status, _, payload = read_http_response(
                port, "GET", "/kv/process-key")
            check(status == 200 and payload == value,
                  f"HTTP restart lost the value: status={status}, body={payload!r}")
            return_code = restarted.terminate_group(signal.SIGTERM)
            check(return_code == 0,
                  f"restarted HTTP shutdown returned {return_code}:\n"
                  f"{restarted.log}")
        finally:
            restarted.cleanup()


def test_kv_persistence_and_sigint():
    with tempfile.TemporaryDirectory(prefix="minidb-process-kv-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "kv.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir)
        try:
            command_type, _ = kv_request(port, HTTP_SET, ["ttl-key", "ttl-value"])
            check(command_type == 101, f"KV SET returned command type {command_type}")
            command_type, _ = kv_request(port, HTTP_EXPIRE, ["ttl-key", "60000"])
            check(command_type == 101,
                  f"KV EXPIRE returned command type {command_type}")

            return_code = server.terminate_group(signal.SIGINT)
            check(return_code == 0,
                  f"KV SIGINT shutdown returned {return_code}:\n{server.log}")
            check("checkpoint saved" in server.log,
                  f"KV shutdown did not report a saved RDB:\n{server.log}")
        finally:
            server.cleanup()

        check(os.path.isfile(rdb_path) and os.path.getsize(rdb_path) > 0,
              "KV SIGINT did not create a non-empty RDB snapshot")

        restarted = start_kv(port, rdb_path, temp_dir)
        try:
            command_type, args = kv_request(port, HTTP_GET, ["ttl-key"])
            check(command_type == 102 and args == [b"ttl-value"],
                  f"KV restart lost the value: type={command_type}, args={args!r}")
            command_type, args = kv_request(port, HTTP_TTL, ["ttl-key"])
            check(command_type == 104 and len(args) == 1,
                  f"KV TTL returned type={command_type}, args={args!r}")
            ttl = int(args[0])
            check(0 < ttl <= 60000,
                  f"restored KV TTL {ttl} is outside (0, 60000]")
            return_code = restarted.terminate_group(signal.SIGTERM)
            check(return_code == 0,
                  f"restarted KV shutdown returned {return_code}:\n"
                  f"{restarted.log}")
        finally:
            restarted.cleanup()


def test_ttl_scanner_lifecycle_and_restart():
    with tempfile.TemporaryDirectory(prefix="minidb-process-ttl-") as temp_dir:
        # Both production servers must own a live background TTL scanner.
        http_rdb = os.path.join(temp_dir, "http-ttl.rdb")
        http_port = free_loopback_port()
        http_server = start_http(http_port, http_rdb, temp_dir)
        try:
            thread_count = wait_for_thread_count(http_server.proc.pid, 2)
            check(thread_count >= 2,
                  f"HTTP server has only {thread_count} thread(s)")
            status, _, payload = read_http_response(
                http_port, "POST", "/kv/http-ttl-key", b"http-value")
            check(status == 200 and payload == b"OK\n",
                  f"HTTP setup failed: status={status}, body={payload!r}")
            check(http_server.terminate_group(signal.SIGTERM) == 0,
                  f"HTTP scanner shutdown failed:\n{http_server.log}")
        finally:
            http_server.cleanup()

        restarted_http = start_http(http_port, http_rdb, temp_dir)
        try:
            status, _, payload = read_http_response(
                http_port, "GET", "/kv/http-ttl-key")
            check(status == 200 and payload == b"http-value",
                  f"HTTP restart after scanner shutdown failed: "
                  f"status={status}, body={payload!r}")
            check(restarted_http.terminate_group(signal.SIGTERM) == 0,
                  f"restarted HTTP shutdown failed:\n{restarted_http.log}")
        finally:
            restarted_http.cleanup()

        kv_rdb = os.path.join(temp_dir, "kv-ttl.rdb")
        kv_port = free_loopback_port()
        kv_server = start_kv(kv_port, kv_rdb, temp_dir)
        try:
            thread_count = wait_for_thread_count(kv_server.proc.pid, 2)
            check(thread_count >= 2,
                  f"KV server has only {thread_count} thread(s)")
            for key, value in (("persistent", b"keep"), ("expiring", b"drop")):
                command_type, _ = kv_request(kv_port, HTTP_SET, [key, value])
                check(command_type == 101,
                      f"KV SET {key} returned {command_type}")
            command_type, _ = kv_request(
                kv_port, HTTP_EXPIRE, ["expiring", "150"])
            check(command_type == 101,
                  f"KV EXPIRE returned {command_type}")

            # Production uses a one-second scanner interval.  Waiting beyond
            # two intervals proves the scanner had an opportunity to reap.
            time.sleep(2.2)
            command_type, args = kv_request(kv_port, HTTP_TTL, ["expiring"])
            check(command_type == 104 and args == [b"-2"],
                  f"expired TTL key still exists: type={command_type}, args={args!r}")
            command_type, args = kv_request(kv_port, HTTP_GET, ["persistent"])
            check(command_type == 102 and args == [b"keep"],
                  f"persistent key disappeared: type={command_type}, args={args!r}")

            started = time.monotonic()
            check(kv_server.terminate_group(signal.SIGTERM) == 0,
                  f"KV scanner shutdown failed:\n{kv_server.log}")
            elapsed = time.monotonic() - started
            check(elapsed < 5.0,
                  f"KV shutdown took {elapsed:.2f}s; scanner may not be joined")
        finally:
            kv_server.cleanup()

        restarted_kv = start_kv(kv_port, kv_rdb, temp_dir)
        try:
            command_type, args = kv_request(kv_port, HTTP_GET, ["persistent"])
            check(command_type == 102 and args == [b"keep"],
                  f"persistent KV key lost after restart: "
                  f"type={command_type}, args={args!r}")
            command_type, args = kv_request(kv_port, HTTP_TTL, ["expiring"])
            check(command_type == 104 and args == [b"-2"],
                  f"expired KV key reappeared after restart: "
                  f"type={command_type}, args={args!r}")
            check(restarted_kv.terminate_group(signal.SIGTERM) == 0,
                  f"restarted KV shutdown failed:\n{restarted_kv.log}")
        finally:
            restarted_kv.cleanup()


def test_corrupt_rdb_startup_fails_without_overwrite():
    with tempfile.TemporaryDirectory(prefix="minidb-process-corrupt-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "corrupt.rdb")
        seed_port = free_loopback_port()
        seed = start_http(seed_port, rdb_path, temp_dir)
        try:
            status, _, payload = read_http_response(
                seed_port, "POST", "/kv/corrupt-seed", b"seed")
            check(status == 200 and payload == b"OK\n",
                  "failed to create a valid seed RDB")
            check(seed.terminate_group(signal.SIGTERM) == 0,
                  f"seed server shutdown failed:\n{seed.log}")
        finally:
            seed.cleanup()

        with open(rdb_path, "r+b") as rdb_file:
            data = bytearray(rdb_file.read())
            check(len(data) >= 5, "seed RDB is unexpectedly short")
            data[-1] ^= 0xFF
            rdb_file.seek(0)
            rdb_file.write(data)
            rdb_file.truncate()
        before = hashlib.sha256(data).hexdigest()

        for binary, label in ((HTTP_BIN, "HTTP"), (KV_BIN, "KV")):
            port = free_loopback_port()
            proc = subprocess.run(
                [binary, "-p", str(port), "-r", rdb_path],
                cwd=temp_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=10.0,
                start_new_session=True,
            )
            check(proc.returncode == 1,
                  f"{label} accepted a corrupt RDB with exit {proc.returncode}:\n"
                  f"{proc.stdout}")
            check("fail" in proc.stdout.lower() or "corrupt" in proc.stdout.lower(),
                  f"{label} did not explain the corrupt RDB failure:\n{proc.stdout}")
            assert_listener_absent(port)
            after = hashlib.sha256(open(rdb_path, "rb").read()).hexdigest()
            check(after == before,
                  f"{label} modified the corrupt RDB during failed startup")


def test_save_failure_exit_code():
    with tempfile.TemporaryDirectory(prefix="minidb-process-savefail-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "savefail.rdb")
        port = free_loopback_port()
        server = start_http(port, rdb_path, temp_dir)
        original_mode = os.stat(temp_dir).st_mode
        try:
            status, _, payload = read_http_response(
                port, "POST", "/kv/savefail-key", b"value")
            check(status == 200 and payload == b"OK\n",
                  "failed to populate the server before save-failure test")
            os.chmod(temp_dir, 0o500)
            status, _, payload = read_http_response(port, "POST", "/save")
            check(status == 500,
                  f"online save failure returned {status}, expected 500: "
                  f"body={payload!r}")
            check(b"failed to save rdb" in payload.lower(),
                  f"online save failure omitted its cause: body={payload!r}")
            return_code = server.terminate_group(signal.SIGTERM)
            check(return_code == 1,
                  f"save failure returned {return_code}, expected 1:\n{server.log}")
            check("failed to publish checkpoint" in server.log.lower(),
                  f"save failure was not reported:\n{server.log}")
        finally:
            os.chmod(temp_dir, original_mode)
            server.cleanup()


def test_http_sync_crash_recovery():
    with tempfile.TemporaryDirectory(prefix="minidb-process-http-crash-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "http-sync-crash.rdb")
        port = free_loopback_port()
        expected = {
            "sync-a": b"alpha\x00value",
            "sync-b": b"beta\xffvalue",
            "sync-c": b"",
        }
        server = start_http(port, rdb_path, temp_dir, "sync")
        try:
            for key, value in expected.items():
                status, _, payload = read_http_response(
                    port, "POST", f"/kv/{key}", value)
                check(status == 200 and payload == b"OK\n",
                      f"HTTP SYNC setup failed for {key}: "
                      f"status={status}, body={payload!r}")
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_http(port, rdb_path, temp_dir, "sync")
        try:
            for key, value in expected.items():
                status, _, payload = read_http_response(
                    port, "GET", f"/kv/{key}")
                check(status == 200 and payload == value,
                      f"HTTP SYNC acknowledged write {key} was lost: "
                      f"status={status}, body={payload!r}")
            stats = read_http_stats(port)
            check(stats["recovery"]["wal_records_replayed"] >= len(expected),
                  f"HTTP recovery did not replay all acknowledged writes: {stats!r}")
            check(stats["last_lsn"] >= len(expected),
                  f"HTTP recovery LSN moved backwards: {stats!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"HTTP recovery shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_kv_sync_crash_recovery():
    with tempfile.TemporaryDirectory(prefix="minidb-process-kv-crash-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "kv-sync-crash.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            for key, value in (("keep", b"value\x00one"), ("drop", b"old")):
                command_type, _ = kv_request(port, HTTP_SET, [key, value])
                check(command_type == 101,
                      f"KV SET {key} returned {command_type}")
            command_type, args = kv_request(port, KV_DEL, ["drop"])
            check(command_type == 104 and args == [b"1"],
                  f"KV DEL returned type={command_type}, args={args!r}")
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, args = kv_request(port, HTTP_GET, ["keep"])
            check(command_type == 102 and args == [b"value\x00one"],
                  f"KV acknowledged SET was lost: type={command_type}, args={args!r}")
            command_type, args = kv_request(port, HTTP_GET, ["drop"])
            check(command_type == 103,
                  f"KV acknowledged DEL was resurrected: "
                  f"type={command_type}, args={args!r}")
            stats = read_kv_stats(port)
            check(stats["recovery"]["wal_records_replayed"] >= 3,
                  f"KV recovery did not replay the write chain: {stats!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"KV recovery shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_interval_flush_recovery():
    with tempfile.TemporaryDirectory(prefix="minidb-process-interval-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "interval.rdb")
        port = free_loopback_port()
        expected = {f"interval-{i}": f"value-{i}".encode() for i in range(4)}
        server = start_kv(port, rdb_path, temp_dir, "interval")
        try:
            for key, value in expected.items():
                command_type, _ = kv_request(port, HTTP_SET, [key, value])
                check(command_type == 101,
                      f"INTERVAL SET {key} returned {command_type}")
            stats = wait_until(
                lambda: (lambda current: current if
                         current["persistence"]["interval_flushes"] >= 1
                         and current["wal"]["data_fsyncs"] >= 1 else None)(
                             read_kv_stats(port)),
                timeout=5.0,
                message="INTERVAL timer did not flush WAL",
            )
            check(stats["durability_mode"] == "interval",
                  f"INTERVAL mode was not reported: {stats!r}")
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_kv(port, rdb_path, temp_dir, "interval")
        try:
            for key, value in expected.items():
                command_type, args = kv_request(port, HTTP_GET, [key])
                check(command_type == 102 and args == [value],
                      f"INTERVAL flushed write {key} was lost: "
                      f"type={command_type}, args={args!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"INTERVAL recovery shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_writer_lock_conflict():
    with tempfile.TemporaryDirectory(prefix="minidb-process-lock-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "locked.rdb")
        first_port = free_loopback_port()
        second_port = free_loopback_port()
        first = start_http(first_port, rdb_path, temp_dir, "sync")
        try:
            result = run_cli(
                HTTP_BIN, ["-p", str(second_port), "-r", rdb_path], temp_dir)
            check(result.returncode == 1,
                  f"second owner exited {result.returncode}, expected 1:\n"
                  f"{result.stdout}")
            output = result.stdout.lower()
            check("already owned" in output or "lock" in output,
                  f"second owner did not report the writer lock:\n{result.stdout}")
            assert_listener_absent(second_port)
            status, _, payload = read_http_response(
                first_port, "GET", "/health")
            check(status == 200 and payload == b"OK\n",
                  "first owner stopped serving after lock conflict")
            check(first.terminate_group(signal.SIGTERM) == 0,
                  f"first owner shutdown failed:\n{first.log}")
        finally:
            first.cleanup()


def test_wal_tail_truncation():
    with tempfile.TemporaryDirectory(prefix="minidb-process-tail-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "tail.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, _ = kv_request(
                port, HTTP_SET, ["tail-key", b"tail-value"])
            check(command_type == 101,
                  f"WAL tail seed returned {command_type}")
            server.kill_group()
        finally:
            server.cleanup()

        segment = wal_segments(rdb_path)[-1]
        record_prefix = segment.read_bytes()[:19]
        check(len(record_prefix) == 19,
              "WAL segment is too short to forge a torn header")
        with open(segment, "ab") as wal_file:
            wal_file.write(record_prefix)
            wal_file.flush()
            os.fsync(wal_file.fileno())

        restarted = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, args = kv_request(port, HTTP_GET, ["tail-key"])
            check(command_type == 102 and args == [b"tail-value"],
                  f"torn-tail recovery lost the preceding record: "
                  f"type={command_type}, args={args!r}")
            stats = read_kv_stats(port)
            check(stats["recovery"]["truncated_bytes"] >= 19,
                  f"torn tail was not reported as truncated: {stats!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"torn-tail shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_manifest_corruption():
    with tempfile.TemporaryDirectory(prefix="minidb-process-manifest-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "manifest.rdb")
        port = free_loopback_port()
        seed = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, _ = kv_request(
                port, HTTP_SET, ["manifest-seed", b"value"])
            check(command_type == 101,
                  f"manifest seed SET returned {command_type}")
            check(seed.terminate_group(signal.SIGTERM) == 0,
                  f"manifest seed shutdown failed:\n{seed.log}")
        finally:
            seed.cleanup()

        manifest_path = rdb_path + ".manifest"
        data = bytearray(Path(manifest_path).read_bytes())
        check(len(data) >= 12, "manifest seed file is unexpectedly short")
        data[-1] ^= 0xFF
        Path(manifest_path).write_bytes(data)
        before = sha256_path(manifest_path)

        for binary, label in ((HTTP_BIN, "HTTP"), (KV_BIN, "KV")):
            failure_port = free_loopback_port()
            expect_startup_failure(
                binary, label, failure_port, rdb_path, temp_dir, "manifest")
            check(sha256_path(manifest_path) == before,
                  f"{label} modified the corrupt manifest during failed startup")


def test_missing_wal_segment():
    with tempfile.TemporaryDirectory(prefix="minidb-process-missing-wal-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "missing-wal.rdb")
        port = free_loopback_port()
        seed = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, _ = kv_request(
                port, HTTP_SET, ["missing-seed", b"value"])
            check(command_type == 101,
                  f"missing-segment seed SET returned {command_type}")
            check(seed.terminate_group(signal.SIGTERM) == 0,
                  f"missing-segment seed shutdown failed:\n{seed.log}")
        finally:
            seed.cleanup()

        segment = wal_segments(rdb_path)[-1]
        segment.unlink()
        manifest_path = rdb_path + ".manifest"
        before = sha256_path(manifest_path)

        for binary, label in ((HTTP_BIN, "HTTP"), (KV_BIN, "KV")):
            failure_port = free_loopback_port()
            expect_startup_failure(
                binary, label, failure_port, rdb_path, temp_dir, "wal")
            check(sha256_path(manifest_path) == before,
                  f"{label} modified the manifest with a missing WAL segment")
            check(not segment.exists(),
                  f"{label} silently recreated the missing WAL segment")


def test_wal_middle_corruption():
    with tempfile.TemporaryDirectory(prefix="minidb-process-mid-wal-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "middle.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, _ = kv_request(
                port, HTTP_SET, ["middle-key-a", b"a" * 96])
            check(command_type == 101,
                  f"middle-corruption first SET returned {command_type}")
            command_type, _ = kv_request(
                port, HTTP_SET, ["middle-key-b", b"b" * 96])
            check(command_type == 101,
                  f"middle-corruption second SET returned {command_type}")
            server.kill_group()
        finally:
            server.cleanup()

        segment = wal_segments(rdb_path)[-1]
        data = bytearray(segment.read_bytes())
        check(len(data) > 64, "WAL segment is too short for corruption test")
        data[40] ^= 0xFF
        segment.write_bytes(data)
        before = sha256_path(segment)

        for binary, label in ((HTTP_BIN, "HTTP"), (KV_BIN, "KV")):
            failure_port = free_loopback_port()
            expect_startup_failure(
                binary, label, failure_port, rdb_path, temp_dir, "corrupt")
            check(sha256_path(segment) == before,
                  f"{label} rewrote the middle-corrupt WAL segment")


def test_legacy_import():
    with tempfile.TemporaryDirectory(prefix="minidb-process-legacy-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "legacy.rdb")
        port = free_loopback_port()
        seed = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, _ = kv_request(
                port, HTTP_SET, ["legacy-key", b"legacy-value"])
            check(command_type == 101,
                  f"legacy seed SET returned {command_type}")
            check(seed.terminate_group(signal.SIGTERM) == 0,
                  f"legacy seed shutdown failed:\n{seed.log}")
        finally:
            seed.cleanup()

        Path(rdb_path + ".manifest").unlink()
        Path(rdb_path + ".lock").unlink(missing_ok=True)
        shutil.rmtree(rdb_path + ".wal", ignore_errors=True)

        imported = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, args = kv_request(port, HTTP_GET, ["legacy-key"])
            check(command_type == 102 and args == [b"legacy-value"],
                  f"legacy RDB was not loaded: type={command_type}, args={args!r}")
            stats = read_kv_stats(port)
            check(stats["recovery"]["legacy_import"] is True,
                  f"legacy import was not reported: {stats!r}")
            check(stats["recovery"]["needs_bootstrap"] is True,
                  f"legacy import skipped bootstrap: {stats!r}")
            check(stats["recovery"]["manifest_present"] is False,
                  f"legacy import invented a manifest: {stats!r}")

            command_type, _ = kv_request(
                port, HTTP_SET, ["post-import", b"new-value"])
            check(command_type == 101,
                  f"first write after legacy import returned {command_type}")
            stats = read_kv_stats(port)
            check(stats["recovery"]["needs_bootstrap"] is False,
                  f"bootstrap state was not cleared: {stats!r}")
            check(stats["recovery"]["manifest_present"] is True,
                  f"bootstrap did not publish a manifest: {stats!r}")
            imported.kill_group()
        finally:
            imported.cleanup()

        recovered = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            for key, value in (("legacy-key", b"legacy-value"),
                               ("post-import", b"new-value")):
                command_type, args = kv_request(port, HTTP_GET, [key])
                check(command_type == 102 and args == [value],
                      f"legacy bootstrap lost {key}: "
                      f"type={command_type}, args={args!r}")
            check(recovered.terminate_group(signal.SIGTERM) == 0,
                  f"legacy recovery shutdown failed:\n{recovered.log}")
        finally:
            recovered.cleanup()


def test_random_kill_seed():
    seed = 20261111
    rng = random.Random(seed)
    target = rng.randint(24, 64)
    with tempfile.TemporaryDirectory(prefix="minidb-process-random-kill-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "random.rdb")
        port = free_loopback_port()
        acknowledged = []
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            for index in range(target):
                key = f"random-{index:03d}"
                value = bytes(rng.randrange(0, 256)
                              for _ in range(rng.randint(16, 96)))
                command_type, _ = kv_request(port, HTTP_SET, [key, value])
                check(command_type == 101,
                      f"random SET {key} returned {command_type} (seed={seed})")
                acknowledged.append((key, value))
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            for key, value in acknowledged:
                command_type, args = kv_request(port, HTTP_GET, [key])
                check(command_type == 102 and args == [value],
                      f"seed={seed} lost acknowledged {key}: "
                      f"type={command_type}, args={args!r}")
            stats = read_kv_stats(port)
            check(stats["last_lsn"] >= target,
                  f"seed={seed} recovered an older LSN: {stats!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"seed={seed} shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_ttl_crash_recovery():
    with tempfile.TemporaryDirectory(prefix="minidb-process-ttl-crash-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "ttl-crash.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, _ = kv_request(
                port, HTTP_SET, ["ttl-crash-key", b"alive"])
            check(command_type == 101,
                  f"TTL crash seed SET returned {command_type}")
            command_type, _ = kv_request(
                port, HTTP_EXPIRE, ["ttl-crash-key", "5000"])
            check(command_type == 101,
                  f"TTL crash EXPIRE returned {command_type}")
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, args = kv_request(
                port, HTTP_GET, ["ttl-crash-key"])
            check(command_type == 102 and args == [b"alive"],
                  f"TTL key was lost across crash: "
                  f"type={command_type}, args={args!r}")
            command_type, args = kv_request(
                port, HTTP_TTL, ["ttl-crash-key"])
            check(command_type == 104 and len(args) == 1,
                  f"TTL after crash returned type={command_type}, args={args!r}")
            remaining = int(args[0])
            check(0 < remaining <= 5000,
                  f"TTL after crash is {remaining}, expected (0, 5000]")
            wait_until(
                lambda: kv_request(port, HTTP_GET, ["ttl-crash-key"])[0] == 103,
                timeout=7.0,
                message="restored TTL key did not expire",
            )
            command_type, _ = kv_request(
                port, HTTP_TTL, ["ttl-crash-key"])
            check(command_type == 104,
                  f"expired TTL query returned {command_type}")
            restarted.kill_group()
        finally:
            restarted.cleanup()

        final = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, args = kv_request(port, HTTP_GET, ["ttl-crash-key"])
            check(command_type == 103,
                  f"expired TTL key reappeared after second crash: "
                  f"type={command_type}, args={args!r}")
            check(final.terminate_group(signal.SIGTERM) == 0,
                  f"TTL final shutdown failed:\n{final.log}")
        finally:
            final.cleanup()


def test_stats_contract():
    with tempfile.TemporaryDirectory(prefix="minidb-process-stats-") as temp_dir:
        http_rdb = os.path.join(temp_dir, "stats-http.rdb")
        http_port = free_loopback_port()
        http_server = start_http(http_port, http_rdb, temp_dir, "sync")
        try:
            status, _, payload = read_http_response(
                http_port, "POST", "/kv/stats-key", b"stats-value")
            check(status == 200 and payload == b"OK\n",
                  f"HTTP stats setup failed: status={status}, body={payload!r}")
            stats = read_http_stats(http_port)
            check(stats["durability_mode"] == "sync",
                  f"HTTP stats omitted sync mode: {stats!r}")
            check(stats["opened"] is True and stats["failed"] is False,
                  f"HTTP stats reported an unhealthy state: {stats!r}")
            check(stats["last_lsn"] == 1 and stats["next_lsn"] == 2,
                  f"HTTP stats LSN is inconsistent: {stats!r}")
            check(stats["wal_bytes"] > 0 and stats["wal_segment_count"] == 1,
                  f"HTTP stats omitted WAL state: {stats!r}")
            check(stats["persistence"]["mutations"] == 1
                  and stats["persistence"]["sync_mutations"] == 1,
                  f"HTTP mutation counters are inconsistent: {stats!r}")
            check(stats["wal"]["appends"] == 1
                  and stats["wal"]["data_fsyncs"] >= 1,
                  f"HTTP WAL counters are inconsistent: {stats!r}")
            check_timing_contract(stats, "HTTP")
            check(stats["recovery"]["manifest_present"] is True,
                  f"HTTP stats omitted manifest state: {stats!r}")

            status, headers, payload = read_http_response(
                http_port, "POST", "/stats")
            check(status == 405 and headers.get("allow") == "GET",
                  f"POST /stats returned {status}, headers={headers!r}, "
                  f"body={payload!r}")
            check(http_server.terminate_group(signal.SIGTERM) == 0,
                  f"HTTP stats shutdown failed:\n{http_server.log}")
        finally:
            http_server.cleanup()

        kv_rdb = os.path.join(temp_dir, "stats-kv.rdb")
        kv_port = free_loopback_port()
        kv_server = start_kv(kv_port, kv_rdb, temp_dir, "sync")
        try:
            command_type, _ = kv_request(
                kv_port, HTTP_SET, ["stats-kv-key", b"stats-kv-value"])
            check(command_type == 101,
                  f"KV stats setup returned {command_type}")
            stats = read_kv_stats(kv_port)
            check(stats["durability_mode"] == "sync",
                  f"KV stats omitted sync mode: {stats!r}")
            check(stats["last_lsn"] == 1 and stats["next_lsn"] == 2,
                  f"KV stats LSN is inconsistent: {stats!r}")
            check(stats["persistence"]["mutations"] == 1
                  and stats["wal"]["appends"] == 1,
                  f"KV stats counters are inconsistent: {stats!r}")
            check_timing_contract(stats, "KV")
            check(stats["wal_bytes"] > 0,
                  f"KV stats omitted WAL bytes: {stats!r}")

            command_type, args = kv_request(
                kv_port, KV_STATS, ["unexpected"])
            check(command_type == 100 and args,
                  f"KV STATS with an argument returned "
                  f"type={command_type}, args={args!r}")
            check(kv_server.terminate_group(signal.SIGTERM) == 0,
                  f"KV stats shutdown failed:\n{kv_server.log}")
        finally:
            kv_server.cleanup()


CASES = {
    "CliContract": test_cli_contract,
    "CliDurabilityContract": test_cli_durability_contract,
    "HttpPersistence": test_http_persistence_and_sigterm,
    "KvPersistence": test_kv_persistence_and_sigint,
    "TtlScannerLifecycle": test_ttl_scanner_lifecycle_and_restart,
    "CorruptRdbStartup": test_corrupt_rdb_startup_fails_without_overwrite,
    "SaveFailureExit": test_save_failure_exit_code,
    "HttpSyncCrashRecovery": test_http_sync_crash_recovery,
    "KvSyncCrashRecovery": test_kv_sync_crash_recovery,
    "IntervalFlushRecovery": test_interval_flush_recovery,
    "WriterLockConflict": test_writer_lock_conflict,
    "WalTailTruncation": test_wal_tail_truncation,
    "ManifestCorruption": test_manifest_corruption,
    "MissingWalSegment": test_missing_wal_segment,
    "WalMiddleCorruption": test_wal_middle_corruption,
    "LegacyImport": test_legacy_import,
    "RandomKillSeed": test_random_kill_seed,
    "TtlCrashRecovery": test_ttl_crash_recovery,
    "StatsContract": test_stats_contract,
}


def main():
    global HTTP_BIN, KV_BIN
    parser = argparse.ArgumentParser()
    parser.add_argument("--http", required=True)
    parser.add_argument("--kv", required=True)
    parser.add_argument("--case", choices=sorted(CASES), required=True)
    args = parser.parse_args()
    HTTP_BIN = os.path.abspath(args.http)
    KV_BIN = os.path.abspath(args.kv)
    try:
        CASES[args.case]()
    except Exception:
        print(f"FAIL {args.case}", flush=True)
        traceback.print_exc()
        return 1
    print(f"PASS {args.case}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
