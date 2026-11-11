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
KV_BEGIN = 9
KV_COMMIT = 10
KV_ROLLBACK = 11
KV_AGENT = 12
KV_ERROR = 100
KV_OK = 101
KV_VALUE = 102
KV_CONFLICT = 105

AGENT_OK = 0
AGENT_CREATED = 1
AGENT_FENCED = 7
AGENT_TOOL_RUNNING = 2
AGENT_RESERVED_PREFIX = b"\x00minidb-agent-v1\x00"


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


class HttpSocketReader:
    def __init__(self, sock, timeout=10.0):
        self.sock = sock
        self.timeout = timeout
        self.buffer = b""
        self.eof = False

    def _receive_more(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TestError("timed out reading HTTP response")
        self.sock.settimeout(remaining)
        chunk = self.sock.recv(65536)
        if not chunk:
            self.eof = True
            return False
        self.buffer += chunk
        return True

    def read_response(self, timeout=None):
        if timeout is None:
            timeout = self.timeout
        deadline = time.monotonic() + timeout
        while b"\r\n\r\n" not in self.buffer:
            if not self._receive_more(deadline):
                raise TestError("EOF before HTTP headers completed")

        raw_headers, payload = self.buffer.split(b"\r\n\r\n", 1)
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
            if not self._receive_more(deadline):
                raise TestError("EOF before HTTP body completed")
            raw_headers, payload = self.buffer.split(b"\r\n\r\n", 1)

        check(len(payload) >= content_length,
              f"HTTP body length {len(payload)} < Content-Length {content_length}")
        body = payload[:content_length]
        self.buffer = payload[content_length:]
        return status, headers, body

    def wait_eof(self, timeout=None):
        if timeout is None:
            timeout = self.timeout
        deadline = time.monotonic() + timeout
        while not self.eof:
            if self.buffer:
                self.buffer = b""
            self._receive_more(deadline)
        return True


def http_request_bytes(port, method, path, body=b"", connection="keep-alive"):
    request = (
        f"{method} {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        f"Connection: {connection}\r\n"
    ).encode("ascii")
    if method == "POST" or body:
        request += f"Content-Length: {len(body)}\r\n".encode("ascii")
    return request + b"\r\n" + body


def connect_http(port, timeout=10.0):
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def reset_socket(sock):
    sock.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_LINGER,
        struct.pack("ii", 1, 0),
    )
    sock.close()


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


def agent_request(port, subcommand, args=(), timeout=5.0):
    return kv_request(port, KV_AGENT, [subcommand, *args], timeout=timeout)


def decode_agent_response(encoded):
    check(isinstance(encoded, bytes) and len(encoded) >= 36,
          f"agent response is truncated: {encoded!r}")
    (magic, version, status, flags, reserved, revision, fence, lsn,
     payload_length) = struct.unpack(">4sBBBBQQQI", encoded[:36])
    check(magic == b"MDOR", f"agent response magic is {magic!r}")
    check(version == 1, f"agent response version is {version}")
    check(flags == 0 and reserved == 0,
          f"agent response flags are invalid: {flags}, {reserved}")
    check(len(encoded) == 36 + payload_length,
          f"agent response length {len(encoded)} != {36 + payload_length}")
    return {
        "status": status,
        "revision": revision,
        "fence": fence,
        "lsn": lsn,
        "payload": encoded[36:],
    }


def decode_agent_record(encoded):
    check(isinstance(encoded, bytes) and len(encoded) >= 52,
          f"agent record is truncated: {encoded!r}")
    (magic, version, record_type, status, reserved, revision, fence,
     created_at_ms, updated_at_ms, lease_expires_at_ms,
     payload_length) = struct.unpack(">4sBBBBQQqqqI", encoded[:52])
    check(magic == b"MDAR", f"agent record magic is {magic!r}")
    check(version == 1, f"agent record version is {version}")
    check(reserved == 0, f"agent record reserved byte is {reserved}")
    check(len(encoded) == 52 + payload_length,
          f"agent record length {len(encoded)} != {52 + payload_length}")
    return {
        "type": record_type,
        "status": status,
        "revision": revision,
        "fence": fence,
        "created_at_ms": created_at_ms,
        "updated_at_ms": updated_at_ms,
        "lease_expires_at_ms": lease_expires_at_ms,
        "payload": encoded[52:],
    }


def agent_value_request(port, subcommand, args=(), timeout=5.0):
    command_type, response_args = agent_request(
        port, subcommand, args, timeout=timeout)
    check(command_type == KV_VALUE and len(response_args) == 1,
          f"AGENT {subcommand} returned type={command_type}, "
          f"args={response_args!r}")
    return decode_agent_response(response_args[0])


class ServerProcess:
    def __init__(self, executable, port, rdb_path=None, cwd=None,
                 durability_mode=None, env_overrides=None):
        command = [executable, "-p", str(port)]
        if rdb_path is not None:
            command += ["-r", rdb_path]
        if durability_mode is not None:
            command += ["-d", durability_mode]
        process_env = None
        if env_overrides:
            process_env = os.environ.copy()
            process_env.update(env_overrides)
        self.proc = subprocess.Popen(
            command,
            cwd=cwd,
            env=process_env,
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


def start_http(port, rdb_path, cwd, durability_mode=None, env_overrides=None):
    server = ServerProcess(HTTP_BIN, port, rdb_path, cwd, durability_mode,
                           env_overrides)
    try:
        server.wait_for_http(port)
    except Exception:
        server.cleanup()
        raise
    return server


def start_kv(port, rdb_path, cwd, durability_mode=None, env_overrides=None):
    server = ServerProcess(KV_BIN, port, rdb_path, cwd, durability_mode,
                           env_overrides)
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


def check_transaction_contract(stats, label):
    transactions = stats.get("transactions")
    check(isinstance(transactions, dict),
          f"{label} stats omitted transactions: {stats!r}")
    required = (
        "active", "started", "committed", "conflicted", "rolled_back",
        "failed", "write_conflicts", "max_active", "versions_current",
        "versions_max", "versions_reclaimed", "oldest_snapshot_lag_lsn",
        "max_oldest_snapshot_lag_lsn", "commit_records", "commit_bytes",
        "commit_total_us", "commit_max_us", "gc_total_us", "gc_max_us",
    )
    for name in required:
        check(isinstance(transactions.get(name), int) and
              transactions[name] >= 0,
              f"{label} transaction field {name} is invalid: {stats!r}")
    check(transactions["active"] <= transactions["max_active"],
          f"{label} active transactions exceed max_active: {stats!r}")
    check(transactions["versions_current"] <= transactions["versions_max"],
          f"{label} current versions exceed versions_max: {stats!r}")
    check(transactions["oldest_snapshot_lag_lsn"] <=
          transactions["max_oldest_snapshot_lag_lsn"],
          f"{label} snapshot lag exceeds its maximum: {stats!r}")
    check(transactions["commit_total_us"] >= transactions["commit_max_us"],
          f"{label} commit timing is inconsistent: {stats!r}")
    check(transactions["gc_total_us"] >= transactions["gc_max_us"],
          f"{label} GC timing is inconsistent: {stats!r}")


def check_agent_contract(stats, label):
    agent = stats.get("agent")
    check(isinstance(agent, dict), f"{label} stats omitted agent: {stats!r}")
    required = (
        "sessions_stored", "sessions_live", "sessions_created",
        "sessions_completed", "sessions_failed", "sessions_cancelled",
        "sessions_purged", "tools_stored", "tools_pending", "tools_running",
        "tools_succeeded", "tools_failed", "tools_cancelled",
        "records_stored", "idempotency_stored", "logical_bytes",
        "logical_bytes_max", "idempotency_replays", "idempotency_conflicts",
        "revision_mismatches", "lease_acquires", "lease_renews",
        "lease_expiries", "fenced_rejections", "gc_runs", "gc_candidates",
        "gc_deleted", "gc_failures", "gc_total_us", "gc_max_us",
    )
    for name in required:
        value = agent.get(name)
        check(isinstance(value, int) and not isinstance(value, bool) and
              value >= 0,
              f"{label} agent field {name} is invalid: {stats!r}")
    check(agent["sessions_live"] <= agent["sessions_stored"],
          f"{label} live sessions exceed stored sessions: {stats!r}")
    check(agent["logical_bytes"] <= agent["logical_bytes_max"],
          f"{label} agent logical bytes exceed the limit: {stats!r}")
    check(agent["gc_total_us"] >= agent["gc_max_us"],
          f"{label} agent GC timing is inconsistent: {stats!r}")
    tool_status_total = sum(
        agent[name] for name in (
            "tools_pending", "tools_running", "tools_succeeded",
            "tools_failed", "tools_cancelled",
        )
    )
    check(tool_status_total == agent["tools_stored"],
          f"{label} agent tool status counters are inconsistent: {stats!r}")


def check_timing_contract(stats, label):
    check_transaction_contract(stats, label)
    check_agent_contract(stats, label)
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

    persistence = stats.get("persistence")
    check(isinstance(persistence, dict),
          f"{label} stats omitted persistence: {stats!r}")
    persistence_required = (
        "checkpoint_pause_us",
        "checkpoint_admission_blocked_total_us",
        "checkpoint_admission_blocked_max_us",
        "checkpoint_snapshot_capture_total_us",
        "checkpoint_snapshot_capture_max_us",
        "checkpoint_snapshot_serialize_total_us",
        "checkpoint_snapshot_serialize_max_us",
        "checkpoint_snapshot_write_total_us",
        "checkpoint_snapshot_write_max_us",
        "checkpoint_snapshot_fsync_total_us",
        "checkpoint_snapshot_fsync_max_us",
        "checkpoint_manifest_total_us",
        "checkpoint_wal_cleanup_total_us",
        "checkpoint_total_us",
        "checkpoint_max_us",
        "checkpoint_failures",
        "save_client_latency_total_us",
        "save_client_latency_max_us",
    )
    for name in persistence_required:
        check(isinstance(persistence.get(name), int) and persistence[name] >= 0,
              f"{label} persistence field {name} is invalid: {stats!r}")
    for total, maximum in (
        ("checkpoint_admission_blocked_total_us",
         "checkpoint_admission_blocked_max_us"),
        ("checkpoint_snapshot_capture_total_us",
         "checkpoint_snapshot_capture_max_us"),
        ("checkpoint_snapshot_serialize_total_us",
         "checkpoint_snapshot_serialize_max_us"),
        ("checkpoint_snapshot_write_total_us",
         "checkpoint_snapshot_write_max_us"),
        ("checkpoint_snapshot_fsync_total_us",
         "checkpoint_snapshot_fsync_max_us"),
        ("checkpoint_total_us", "checkpoint_max_us"),
        ("save_client_latency_total_us", "save_client_latency_max_us"),
    ):
        check(persistence[total] >= persistence[maximum],
              f"{label} {total} is smaller than {maximum}: {stats!r}")
    check(persistence.get("background_checkpoint_state") in
          ("idle", "capture", "writing", "finalizing", "cleanup", "failed"),
          f"{label} background checkpoint state is invalid: {stats!r}")

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


def test_shutdown_failure_exit_code():
    with tempfile.TemporaryDirectory(
            prefix="minidb-process-shutdownfail-") as temp_dir:
        cases = (
            ("HTTP", "shutdownfail-http.rdb", start_http, True),
            ("KV", "shutdownfail-kv.rdb", start_kv, False),
        )
        original_mode = os.stat(temp_dir).st_mode
        try:
            for label, filename, starter, is_http in cases:
                rdb_path = os.path.join(temp_dir, filename)
                port = free_loopback_port()
                server = starter(port, rdb_path, temp_dir, "sync")
                try:
                    if is_http:
                        status, _, payload = read_http_response(
                            port, "POST", "/kv/shutdownfail-key", b"value")
                        check(status == 200 and payload == b"OK\n",
                              f"failed to populate {label} before shutdown test")
                    else:
                        command_type, args = kv_request(
                            port, HTTP_SET, ["shutdownfail-key", b"value"])
                        check(command_type == 101 and args == [],
                              f"{label} setup returned "
                              f"type={command_type}, args={args!r}")

                    os.chmod(temp_dir, 0o500)
                    return_code = server.terminate_group(signal.SIGTERM)
                    check(return_code == 1,
                          f"{label} shutdown failure returned {return_code}, "
                          f"expected 1:\n{server.log}")
                    check("failed to publish checkpoint during shutdown"
                          in server.log.lower(),
                          f"{label} shutdown failure was not reported:\n"
                          f"{server.log}")
                finally:
                    os.chmod(temp_dir, original_mode)
                    server.cleanup()
        finally:
            os.chmod(temp_dir, original_mode)


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


def test_background_checkpoint_rename_crash():
    with tempfile.TemporaryDirectory(
            prefix="minidb-process-background-rename-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "background-rename.rdb")
        port = free_loopback_port()
        marker = os.path.join(temp_dir, "rdb-published.marker")
        release = os.path.join(temp_dir, "rdb-published.release")
        env = {
            "MINIDB_TEST_RDB_PUBLISH_MARKER": marker,
            "MINIDB_TEST_RDB_PUBLISH_RELEASE": release,
        }
        server = start_http(port, rdb_path, temp_dir, "sync", env)
        save_socket = None
        try:
            status, _, payload = read_http_response(
                port, "POST", "/kv/before-capture", b"snapshot-value")
            check(status == 200 and payload == b"OK\n",
                  f"background checkpoint seed failed: "
                  f"status={status}, body={payload!r}")

            save_socket = connect_http(port, timeout=20.0)
            save_socket.sendall(
                http_request_bytes(port, "POST", "/save", connection="close"))
            wait_until(lambda: os.path.exists(marker), timeout=10.0,
                       message="checkpoint worker did not reach RDB publish")

            # The RDB rename and directory fsync are complete, but the old
            # manifest is still authoritative. Admission must already be
            # resumed so this second write lands in WAL.
            status, _, payload = read_http_response(
                port, "POST", "/kv/after-capture", b"wal-value")
            check(status == 200 and payload == b"OK\n",
                  f"writes did not resume during background RDB publish: "
                  f"status={status}, body={payload!r}")
            stats = read_http_stats(port)
            check(stats["persistence"]["checkpoints"] == 0,
                  f"checkpoint published before the crash window: {stats!r}")
            check(stats["persistence"]["background_checkpoint_state"] ==
                  "writing",
                  f"checkpoint state did not expose background writing: {stats!r}")
            check(stats["recovery"]["checkpoint_lsn"] == 0,
                  f"old manifest was unexpectedly replaced: {stats!r}")
            server.kill_group()
        finally:
            if save_socket is not None:
                save_socket.close()
            server.cleanup()

        restarted = start_http(port, rdb_path, temp_dir, "sync")
        try:
            for key, value in (
                    ("before-capture", b"snapshot-value"),
                    ("after-capture", b"wal-value")):
                status, _, payload = read_http_response(
                    port, "GET", f"/kv/{key}")
                check(status == 200 and payload == value,
                      f"background checkpoint crash lost {key}: "
                      f"status={status}, body={payload!r}")
            stats = read_http_stats(port)
            check(stats["recovery"]["checkpoint_lsn"] == 0,
                  f"recovery did not honor the old manifest: {stats!r}")
            check(stats["recovery"]["wal_records_replayed"] >= 2,
                  f"recovery did not replay WAL over the new RDB: {stats!r}")

            status, _, payload = read_http_response(
                port, "POST", "/save", timeout=20.0)
            check(status == 200,
                  f"post-recovery SAVE failed: status={status}, body={payload!r}")
            stats = read_http_stats(port)
            check(stats["persistence"]["checkpoints"] == 1,
                  f"post-recovery checkpoint was not recorded: {stats!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"background checkpoint recovery shutdown failed:\n"
                  f"{restarted.log}")
        finally:
            restarted.cleanup()

        # recovery.checkpoint_lsn is a startup-recovery snapshot. Confirm the
        # newly published manifest in a fresh process instead of expecting the
        # running process to rewrite that immutable statistic.
        reopened = start_http(port, rdb_path, temp_dir, "sync")
        try:
            stats = read_http_stats(port)
            check(stats["recovery"]["checkpoint_lsn"] >= 2,
                  f"new manifest was not visible after restart: {stats!r}")
            check(reopened.terminate_group(signal.SIGTERM) == 0,
                  f"post-checkpoint restart shutdown failed:\n{reopened.log}")
        finally:
            reopened.cleanup()


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


def test_kv_async_protocol():
    with tempfile.TemporaryDirectory(prefix="minidb-process-kv-async-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "kv-async.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            commands = [
                (HTTP_SET, [b"kv-pipe", b"first-value"]),
                (HTTP_GET, [b"kv-pipe"]),
                (KV_DEL, [b"kv-pipe"]),
                (HTTP_GET, [b"kv-pipe"]),
                (HTTP_SET, [b"kv-remain", b"remain-value"]),
                (KV_SAVE, []),
                (HTTP_SET, [b"kv-latest", b"after-save"]),
                (HTTP_PING, []),
            ]
            expected = [
                (101, []),
                (102, [b"first-value"]),
                (104, [b"1"]),
                (103, []),
                (101, []),
                (101, []),
                (101, []),
                (101, [b"PONG"]),
            ]
            with socket.create_connection(("127.0.0.1", port), timeout=10.0) as sock:
                sock.settimeout(10.0)
                sock.sendall(b"".join(kv_encode(kind, args)
                                      for kind, args in commands))
                for index, (wanted_type, wanted_args) in enumerate(expected):
                    command_type, args = kv_read_frame(sock)
                    check(command_type == wanted_type and args == wanted_args,
                          f"KV pipeline response {index} was "
                          f"type={command_type}, args={args!r}, expected "
                          f"type={wanted_type}, args={wanted_args!r}")
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, args = kv_request(port, HTTP_GET, ["kv-remain"])
            check(command_type == 102 and args == [b"remain-value"],
                  f"KV pipeline acknowledged SET was lost: "
                  f"type={command_type}, args={args!r}")
            command_type, args = kv_request(port, HTTP_GET, ["kv-latest"])
            check(command_type == 102 and args == [b"after-save"],
                  f"KV post-SAVE acknowledged SET was lost: "
                  f"type={command_type}, args={args!r}")
            command_type, args = kv_request(port, HTTP_GET, ["kv-pipe"])
            check(command_type == 103,
                  f"KV acknowledged DEL was resurrected: "
                  f"type={command_type}, args={args!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"KV async recovery shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_kv_disconnect_safety():
    with tempfile.TemporaryDirectory(prefix="minidb-process-kv-reset-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "kv-reset.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            for index in range(4):
                sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
                sock.settimeout(10.0)
                sock.sendall(kv_encode(
                    HTTP_SET,
                    [f"reset-{index}".encode(),
                     bytes((value * 37 + index) & 0xff for value in range(524288))],
                ))
                reset_socket(sock)

            save_sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
            save_sock.settimeout(10.0)
            save_sock.sendall(kv_encode(KV_SAVE))
            reset_socket(save_sock)

            command_type, args = kv_request(port, HTTP_PING, timeout=10.0)
            check(command_type == 101 and args == [b"PONG"],
                  f"KV server failed after peer reset: "
                  f"type={command_type}, args={args!r}")
            stats = wait_until(
                lambda: (lambda current: current if
                         current["group_commit"]["writer_state"] == "running"
                         else None)(read_kv_stats(port)),
                timeout=5.0,
                message="KV writer did not resume after peer reset",
            )
            check(stats["group_commit"]["writer_failures"] == 0,
                  f"KV writer failed after peer reset: {stats!r}")
            check(server.terminate_group(signal.SIGTERM) == 0,
                  f"KV reset-safe shutdown failed:\n{server.log}")
        finally:
            server.cleanup()


def test_http_async_protocol():
    with tempfile.TemporaryDirectory(prefix="minidb-process-http-async-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "http-async.rdb")
        port = free_loopback_port()
        server = start_http(port, rdb_path, temp_dir, "sync")
        try:
            requests = b"".join([
                http_request_bytes(port, "POST", "/kv/http-pipe", b"http-value"),
                http_request_bytes(port, "GET", "/kv/http-pipe"),
                http_request_bytes(port, "DELETE", "/kv/http-pipe"),
                http_request_bytes(port, "GET", "/kv/http-pipe"),
                http_request_bytes(port, "POST", "/kv/http-remain", b"remain"),
                http_request_bytes(port, "POST", "/save"),
            ])
            expected = [
                (200, b"OK\n"),
                (200, b"http-value"),
                (200, b"OK\n"),
                (404, b"404 Not Found\n"),
                (200, b"OK\n"),
                (200, None),
            ]
            with connect_http(port) as sock:
                sock.sendall(requests)
                reader = HttpSocketReader(sock)
                for index, (wanted_status, wanted_body) in enumerate(expected):
                    status, headers, body = reader.read_response()
                    check(status == wanted_status,
                          f"HTTP pipeline response {index} status was {status}, "
                          f"expected {wanted_status}: {body!r}")
                    if wanted_body is not None:
                        check(body == wanted_body,
                              f"HTTP pipeline response {index} body was "
                              f"{body!r}, expected {wanted_body!r}")
                    else:
                        check(headers.get("content-type") == "application/json",
                              f"HTTP SAVE response content type was "
                              f"{headers.get('content-type')!r}")
                        check(json.loads(body.decode("utf-8"))["status"] == "ok",
                              f"HTTP SAVE response was {body!r}")

            with connect_http(port) as close_sock:
                close_sock.sendall(http_request_bytes(
                    port, "POST", "/kv/http-close", b"close-value",
                    connection="close"))
                close_reader = HttpSocketReader(close_sock)
                status, _, body = close_reader.read_response()
                check(status == 200 and body == b"OK\n",
                      f"HTTP Connection: close POST returned "
                      f"status={status}, body={body!r}")
                check(close_reader.wait_eof(),
                      "HTTP Connection: close response did not reach EOF")
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_http(port, rdb_path, temp_dir, "sync")
        try:
            status, _, payload = read_http_response(port, "GET", "/kv/http-remain")
            check(status == 200 and payload == b"remain",
                  f"HTTP pipeline acknowledged POST was lost: "
                  f"status={status}, body={payload!r}")
            status, _, payload = read_http_response(port, "GET", "/kv/http-close")
            check(status == 200 and payload == b"close-value",
                  f"HTTP Connection: close acknowledged POST was lost: "
                  f"status={status}, body={payload!r}")
            status, _, payload = read_http_response(port, "GET", "/kv/http-pipe")
            check(status == 404,
                  f"HTTP acknowledged DELETE was resurrected: "
                  f"status={status}, body={payload!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"HTTP async recovery shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_http_disconnect_safety():
    with tempfile.TemporaryDirectory(prefix="minidb-process-http-reset-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "http-reset.rdb")
        port = free_loopback_port()
        server = start_http(port, rdb_path, temp_dir, "sync")
        try:
            for index in range(4):
                sock = connect_http(port)
                sock.sendall(http_request_bytes(
                    port, "POST", f"/kv/reset-{index}",
                    bytes((value * 19 + index) & 0xff for value in range(262144))))
                reset_socket(sock)

            save_sock = connect_http(port)
            save_sock.sendall(http_request_bytes(port, "POST", "/save"))
            reset_socket(save_sock)

            status, _, payload = read_http_response(port, "GET", "/health",
                                                    timeout=10.0)
            check(status == 200 and payload == b"OK\n",
                  f"HTTP server failed after peer reset: "
                  f"status={status}, body={payload!r}")
            stats = wait_until(
                lambda: (lambda current: current if
                         current["group_commit"]["writer_state"] == "running"
                         else None)(read_http_stats(port)),
                timeout=5.0,
                message="HTTP writer did not resume after peer reset",
            )
            check(stats["group_commit"]["writer_failures"] == 0,
                  f"HTTP writer failed after peer reset: {stats!r}")
            check(server.terminate_group(signal.SIGTERM) == 0,
                  f"HTTP reset-safe shutdown failed:\n{server.log}")
        finally:
            server.cleanup()


def connect_kv(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
    sock.settimeout(5.0)
    return sock


def kv_command(sock, command_type, args=()):
    sock.sendall(kv_encode(command_type, args))
    return kv_read_frame(sock)


def test_transaction_contract():
    with tempfile.TemporaryDirectory(
            prefix="minidb-process-transactions-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "transactions.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, _ = kv_request(port, HTTP_SET, ["base", "before"])
            check(command_type == 101,
                  f"transaction setup SET returned {command_type}")
            before = read_kv_stats(port)

            writer = connect_kv(port)
            reader = connect_kv(port)
            try:
                command_type, args = kv_command(writer, KV_BEGIN)
                check(command_type == 101 and len(args) == 1,
                      f"BEGIN returned type={command_type}, args={args!r}")
                writer_id = int(args[0].decode("ascii"))
                check(writer_id > 0, f"BEGIN returned invalid id {writer_id}")

                command_type, _ = kv_command(writer, HTTP_SET, ["base", "tx1"])
                check(command_type == 101,
                      f"transaction SET returned {command_type}")
                command_type, args = kv_command(writer, HTTP_GET, ["base"])
                check(command_type == 102 and args == [b"tx1"],
                      f"transaction did not read its own write: "
                      f"type={command_type}, args={args!r}")

                command_type, args = kv_command(reader, KV_BEGIN)
                check(command_type == 101 and len(args) == 1,
                      f"reader BEGIN returned type={command_type}, args={args!r}")
                command_type, args = kv_command(reader, HTTP_GET, ["base"])
                check(command_type == 102 and args == [b"before"],
                      f"reader snapshot was not consistent: "
                      f"type={command_type}, args={args!r}")

                command_type, args = kv_command(writer, KV_COMMIT)
                check(command_type == 101 and args and args[0] == b"2",
                      f"COMMIT returned type={command_type}, args={args!r}")

                command_type, args = kv_command(reader, HTTP_GET, ["base"])
                check(command_type == 102 and args == [b"before"],
                      f"repeatable read changed after commit: "
                      f"type={command_type}, args={args!r}")
                command_type, args = kv_command(reader, KV_COMMIT)
                check(command_type == 101 and args == [b"0"],
                      f"read-only COMMIT returned type={command_type}, "
                      f"args={args!r}")

                first = connect_kv(port)
                second = connect_kv(port)
                try:
                    command_type, args = kv_command(first, KV_BEGIN)
                    check(command_type == 101 and len(args) == 1,
                          f"conflict BEGIN 1 returned {command_type}")
                    command_type, args = kv_command(second, KV_BEGIN)
                    check(command_type == 101 and len(args) == 1,
                          f"conflict BEGIN 2 returned {command_type}")
                    check(kv_command(first, HTTP_SET, ["race", "first"])[0] == 101,
                          "first conflict write failed")
                    check(kv_command(second, HTTP_SET, ["race", "second"])[0] == 101,
                          "second conflict write failed")
                    command_type, _ = kv_command(first, KV_COMMIT)
                    check(command_type == 101,
                          f"first conflict commit returned {command_type}")
                    command_type, _ = kv_command(second, KV_COMMIT)
                    check(command_type == KV_CONFLICT,
                          f"second conflict commit returned {command_type}")
                finally:
                    first.close()
                    second.close()

                command_type, args = kv_command(reader, HTTP_GET, ["race"])
                check(command_type == 102 and args == [b"first"],
                      f"conflict winner is incorrect: "
                      f"type={command_type}, args={args!r}")

                rollback = connect_kv(port)
                try:
                    check(kv_command(rollback, KV_BEGIN)[0] == 101,
                          "rollback BEGIN failed")
                    check(kv_command(rollback, HTTP_SET,
                                     ["rolled-back", "no"])[0] == 101,
                          "rollback SET failed")
                    check(kv_command(rollback, KV_ROLLBACK)[0] == 101,
                          "ROLLBACK failed")
                finally:
                    rollback.close()
                command_type, _ = kv_request(port, HTTP_GET, ["rolled-back"])
                check(command_type == 103,
                      f"rolled-back write became visible: {command_type}")

                errors = connect_kv(port)
                try:
                    check(kv_command(errors, KV_COMMIT)[0] == 100,
                          "COMMIT without a transaction was accepted")
                    check(kv_command(errors, KV_ROLLBACK)[0] == 100,
                          "ROLLBACK without a transaction was accepted")
                    check(kv_command(errors, KV_BEGIN, ["unexpected"])[0] == 100,
                          "BEGIN with an argument was accepted")
                    command_type, args = kv_command(errors, KV_BEGIN)
                    check(command_type == 101 and len(args) == 1,
                          f"BEGIN after errors returned {command_type}")
                    check(kv_command(errors, KV_BEGIN)[0] == 100,
                          "nested BEGIN was accepted")
                    check(kv_command(errors, HTTP_SET, ["only-one"])[0] == 100,
                          "SET with invalid arity was accepted")
                    check(kv_command(errors, HTTP_GET)[0] == 100,
                          "GET with invalid arity was accepted")
                    check(kv_command(errors, HTTP_SET,
                                     ["kept", "value"])[0] == 101,
                          "valid transaction SET failed")
                    check(kv_command(errors, KV_COMMIT, ["unexpected"])[0] == 100,
                          "COMMIT with an argument was accepted")
                    command_type, _ = kv_command(errors, KV_COMMIT)
                    check(command_type == 101,
                          f"COMMIT after recoverable errors returned "
                          f"{command_type}")
                    command_type, args = kv_command(errors, HTTP_GET, ["kept"])
                    check(command_type == 102 and args == [b"value"],
                          f"transaction-state error discarded the write: "
                          f"type={command_type}, args={args!r}")
                    check(kv_command(errors, HTTP_PING)[0] == 101,
                          "connection was unusable after transaction errors")
                finally:
                    errors.close()

                abandoned = connect_kv(port)
                try:
                    check(kv_command(abandoned, KV_BEGIN)[0] == 101,
                          "abandoned BEGIN failed")
                    check(kv_command(abandoned, HTTP_SET,
                                     ["abandoned", "no"])[0] == 101,
                          "abandoned SET failed")
                finally:
                    abandoned.close()
                wait_until(
                    lambda: (read_kv_stats(port)["transactions"]["rolled_back"]
                             >= before["transactions"]["rolled_back"] + 2),
                    5.0,
                    "disconnect did not roll back the active transaction",
                )
                command_type, _ = kv_request(port, HTTP_GET, ["abandoned"])
                check(command_type == 103,
                      f"disconnect-rollback write became visible: {command_type}")

                committing = connect_kv(port)
                try:
                    check(kv_command(committing, KV_BEGIN)[0] == 101,
                          "disconnect-commit BEGIN failed")
                    check(kv_command(committing, HTTP_SET,
                                     ["accepted", "yes"])[0] == 101,
                          "disconnect-commit SET failed")
                    committing.sendall(kv_encode(KV_COMMIT))
                finally:
                    committing.close()
                wait_until(
                    lambda: (read_kv_stats(port)["transactions"]["committed"]
                             >= before["transactions"]["committed"] + 5),
                    5.0,
                    "accepted COMMIT did not complete after disconnect",
                )
                wait_until(
                    lambda: kv_request(port, HTTP_GET, ["accepted"])[0] == 102,
                    5.0,
                    "accepted COMMIT did not publish its write",
                )

                stats = read_kv_stats(port)
                check_transaction_contract(stats, "KV transaction")
                transactions = stats["transactions"]
                started = (transactions["started"]
                           - before["transactions"]["started"])
                committed = (transactions["committed"]
                             - before["transactions"]["committed"])
                check(started == 8,
                      f"unexpected transaction start count: {transactions!r}")
                check(committed == 5,
                      f"unexpected transaction commit count: {transactions!r}")
                check(transactions["conflicted"] -
                      before["transactions"]["conflicted"] == 1,
                      f"conflict counter is incorrect: {transactions!r}")
                check(transactions["rolled_back"] -
                      before["transactions"]["rolled_back"] == 2,
                      f"rollback counter is incorrect: {transactions!r}")
                check(transactions["write_conflicts"] >= 1,
                      f"write conflict counter is incorrect: {transactions!r}")
                check(transactions["active"] == 0,
                      f"active transaction leaked: {transactions!r}")
                check(transactions["max_active"] >= 2,
                      f"max_active did not observe concurrency: {transactions!r}")
                check(transactions["commit_records"] -
                      before["transactions"]["commit_records"] == 4,
                      f"commit record counter is incorrect: {transactions!r}")
                check(transactions["commit_bytes"] >
                      before["transactions"]["commit_bytes"],
                      f"commit byte counter did not advance: {transactions!r}")
            finally:
                writer.close()
                reader.close()
            check(server.terminate_group(signal.SIGTERM) == 0,
                  f"transaction server shutdown failed:\n{server.log}")
        finally:
            server.cleanup()


def test_agent_idempotency_crash_recovery():
    with tempfile.TemporaryDirectory(
            prefix="minidb-process-agent-idempotency-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "agent-idempotency.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            command_type, response_args = agent_request(
                port, "SESSION_CREATE",
                ["idem-session", "60000", "working", "create-idem-session"])
            check(command_type == KV_VALUE and len(response_args) == 1,
                  f"AGENT SESSION_CREATE returned type={command_type}, "
                  f"args={response_args!r}")
            original_bytes = response_args[0]
            original = decode_agent_response(original_bytes)
            check(original["status"] == AGENT_CREATED,
                  f"agent session was not created: {original!r}")
            check(original["revision"] == 1 and original["lsn"] >= 1,
                  f"agent create response is inconsistent: {original!r}")
            before = read_kv_stats(port)
            check(before["last_lsn"] == original["lsn"],
                  f"agent create LSN is inconsistent: {before!r}")
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            recovered = read_kv_stats(port)
            check(recovered["last_lsn"] == before["last_lsn"],
                  f"agent restart changed the durable LSN: {recovered!r}")
            command_type, response_args = agent_request(
                port, "SESSION_CREATE",
                ["idem-session", "60000", "working", "create-idem-session"])
            check(command_type == KV_VALUE and len(response_args) == 1,
                  f"replayed AGENT SESSION_CREATE returned "
                  f"type={command_type}, args={response_args!r}")
            check(response_args[0] == original_bytes,
                  "replayed agent response changed across the crash")
            replay = decode_agent_response(response_args[0])
            check(replay["lsn"] == original["lsn"],
                  f"replayed agent mutation advanced the LSN: {replay!r}")
            after = read_kv_stats(port)
            check(after["last_lsn"] == before["last_lsn"],
                  f"agent replay advanced the durable LSN: {after!r}")
            check(after["agent"]["idempotency_replays"] ==
                  before["agent"]["idempotency_replays"] + 1,
                  f"agent replay counter is incorrect: {after!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"agent idempotency shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_agent_lease_takeover():
    with tempfile.TemporaryDirectory(
            prefix="minidb-process-agent-lease-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "agent-lease.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            created = agent_value_request(
                port, "SESSION_CREATE",
                ["lease-session", "60000", "working", "lease-create"])
            check(created["status"] == AGENT_CREATED,
                  f"lease session was not created: {created!r}")

            first = agent_value_request(
                port, "LEASE_ACQUIRE",
                ["lease-session", "1000", "lease-first"])
            check(first["status"] == AGENT_OK and first["fence"] == 1,
                  f"first lease acquisition is inconsistent: {first!r}")

            tool = agent_value_request(
                port, "TOOL_CREATE",
                ["lease-session", "tool-1", "shell", "echo hi",
                 "lease-tool-create"])
            check(tool["status"] == AGENT_CREATED and tool["revision"] == 1,
                  f"lease tool creation is inconsistent: {tool!r}")

            claim = agent_value_request(
                port, "TOOL_CLAIM",
                ["lease-session", "tool-1", "1", "1", "1000",
                 "lease-claim"])
            check(claim["status"] == AGENT_OK and claim["revision"] == 2 and
                  claim["fence"] == 1,
                  f"initial tool claim is inconsistent: {claim!r}")

            time.sleep(1.5)
            takeover = agent_value_request(
                port, "LEASE_ACQUIRE",
                ["lease-session", "10000", "lease-second"])
            check(takeover["status"] == AGENT_OK and takeover["fence"] == 2,
                  f"lease takeover did not advance the fence: {takeover!r}")

            stale = agent_value_request(
                port, "TOOL_COMPLETE",
                ["lease-session", "tool-1", "2", "1", "SUCCEEDED", "done",
                 "lease-stale-complete"])
            check(stale["status"] == AGENT_FENCED and stale["fence"] == 2,
                  f"stale fence was not rejected: {stale!r}")

            stats = read_kv_stats(port)
            check(stats["agent"]["lease_acquires"] >= 2,
                  f"lease acquisition counter is incorrect: {stats!r}")
            check(stats["agent"]["fenced_rejections"] >= 1,
                  f"fence rejection counter is incorrect: {stats!r}")
            check(server.terminate_group(signal.SIGTERM) == 0,
                  f"agent lease shutdown failed:\n{server.log}")
        finally:
            server.cleanup()


def test_agent_crash_recovery():
    with tempfile.TemporaryDirectory(
            prefix="minidb-process-agent-crash-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "agent-crash.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            created = agent_value_request(
                port, "SESSION_CREATE",
                ["crash-session", "300000", "working", "crash-create"])
            check(created["status"] == AGENT_CREATED,
                  f"crash session was not created: {created!r}")
            lease = agent_value_request(
                port, "LEASE_ACQUIRE",
                ["crash-session", "2000", "crash-lease-one"])
            check(lease["status"] == AGENT_OK and lease["fence"] == 1,
                  f"crash lease acquisition is inconsistent: {lease!r}")
            tool = agent_value_request(
                port, "TOOL_CREATE",
                ["crash-session", "crash-tool", "shell", "echo crash",
                 "crash-tool-create"])
            check(tool["status"] == AGENT_CREATED and tool["revision"] == 1,
                  f"crash tool creation is inconsistent: {tool!r}")
            claim = agent_value_request(
                port, "TOOL_CLAIM",
                ["crash-session", "crash-tool", "1", "1", "2000",
                 "crash-claim-one"])
            check(claim["status"] == AGENT_OK and claim["revision"] == 2,
                  f"crash tool claim is inconsistent: {claim!r}")
            server.kill_group()
        finally:
            server.cleanup()

        restarted = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            recovered = agent_value_request(
                port, "TOOL_GET", ["crash-session", "crash-tool"])
            check(recovered["status"] == AGENT_OK and
                  recovered["revision"] == 2 and recovered["fence"] == 1,
                  f"recovered tool metadata is inconsistent: {recovered!r}")
            recovered_record = decode_agent_record(recovered["payload"])
            check(recovered_record["status"] == AGENT_TOOL_RUNNING,
                  f"recovered tool did not remain RUNNING: "
                  f"{recovered_record!r}")

            time.sleep(2.2)
            takeover = agent_value_request(
                port, "LEASE_ACQUIRE",
                ["crash-session", "10000", "crash-lease-two"])
            check(takeover["status"] == AGENT_OK and takeover["fence"] == 2,
                  f"crash lease takeover is inconsistent: {takeover!r}")

            stale = agent_value_request(
                port, "TOOL_COMPLETE",
                ["crash-session", "crash-tool", "2", "1", "SUCCEEDED",
                 "stale", "crash-stale-complete"])
            check(stale["status"] == AGENT_FENCED and stale["fence"] == 2,
                  f"recovered stale fence was not rejected: {stale!r}")

            requeue = agent_value_request(
                port, "TOOL_REQUEUE",
                ["crash-session", "crash-tool", "2", "2",
                 "crash-requeue"])
            check(requeue["status"] == AGENT_OK and requeue["revision"] == 3,
                  f"tool requeue after recovery failed: {requeue!r}")

            reclaim = agent_value_request(
                port, "TOOL_CLAIM",
                ["crash-session", "crash-tool", "3", "2", "10000",
                 "crash-claim-two"])
            check(reclaim["status"] == AGENT_OK and reclaim["revision"] == 4 and
                  reclaim["fence"] == 2,
                  f"tool reclaim after recovery failed: {reclaim!r}")

            final_tool = agent_value_request(
                port, "TOOL_GET", ["crash-session", "crash-tool"])
            final_record = decode_agent_record(final_tool["payload"])
            check(final_tool["status"] == AGENT_OK and
                  final_tool["revision"] == 4 and final_tool["fence"] == 2 and
                  final_record["status"] == AGENT_TOOL_RUNNING,
                  f"reclaimed tool state is inconsistent: {final_tool!r}")
            check(restarted.terminate_group(signal.SIGTERM) == 0,
                  f"agent crash recovery shutdown failed:\n{restarted.log}")
        finally:
            restarted.cleanup()


def test_agent_namespace_collision():
    with tempfile.TemporaryDirectory(
            prefix="minidb-process-agent-namespace-") as temp_dir:
        rdb_path = os.path.join(temp_dir, "agent-namespace.rdb")
        port = free_loopback_port()
        server = start_kv(port, rdb_path, temp_dir, "sync")
        try:
            reserved_key = AGENT_RESERVED_PREFIX + b"session/id"
            with connect_kv(port) as sock:
                operations = (
                    (HTTP_SET, [reserved_key, b"value"], "SET"),
                    (HTTP_GET, [reserved_key], "GET"),
                    (KV_DEL, [reserved_key], "DEL"),
                )
                for command_type, args, name in operations:
                    response_type, response_args = kv_command(
                        sock, command_type, args)
                    check(response_type == KV_ERROR and response_args,
                          f"ordinary {name} entered the agent namespace: "
                          f"type={response_type}, args={response_args!r}")
                ping_type, ping_args = kv_command(sock, HTTP_PING)
                check(ping_type == KV_OK and ping_args == [b"PONG"],
                      f"agent namespace rejection closed the connection: "
                      f"type={ping_type}, args={ping_args!r}")
            check(server.terminate_group(signal.SIGTERM) == 0,
                  f"agent namespace shutdown failed:\n{server.log}")
        finally:
            server.cleanup()


def test_agent_stats_contract():
    with tempfile.TemporaryDirectory(
            prefix="minidb-process-agent-stats-") as temp_dir:
        http_rdb = os.path.join(temp_dir, "agent-stats-http.rdb")
        http_port = free_loopback_port()
        http_server = start_http(http_port, http_rdb, temp_dir, "sync")
        try:
            stats = read_http_stats(http_port)
            check_agent_contract(stats, "HTTP agent")
            check(http_server.terminate_group(signal.SIGTERM) == 0,
                  f"HTTP agent stats shutdown failed:\n{http_server.log}")
        finally:
            http_server.cleanup()

        kv_rdb = os.path.join(temp_dir, "agent-stats-kv.rdb")
        kv_port = free_loopback_port()
        kv_server = start_kv(kv_port, kv_rdb, temp_dir, "sync")
        try:
            created = agent_value_request(
                kv_port, "SESSION_CREATE",
                ["stats-session", "300000", "working", "stats-create"])
            check(created["status"] == AGENT_CREATED,
                  f"stats session was not created: {created!r}")
            lease = agent_value_request(
                kv_port, "LEASE_ACQUIRE",
                ["stats-session", "10000", "stats-lease"])
            check(lease["status"] == AGENT_OK and lease["fence"] == 1,
                  f"stats lease acquisition failed: {lease!r}")
            tool = agent_value_request(
                kv_port, "TOOL_CREATE",
                ["stats-session", "stats-tool", "shell", "echo stats",
                 "stats-tool-create"])
            check(tool["status"] == AGENT_CREATED,
                  f"stats tool creation failed: {tool!r}")
            claim = agent_value_request(
                kv_port, "TOOL_CLAIM",
                ["stats-session", "stats-tool", "1", "1", "10000",
                 "stats-claim"])
            check(claim["status"] == AGENT_OK,
                  f"stats tool claim failed: {claim!r}")

            stats = read_kv_stats(kv_port)
            check_agent_contract(stats, "KV agent")
            agent = stats["agent"]
            check(agent["sessions_stored"] == 1 and
                  agent["sessions_live"] == 1,
                  f"agent session counters are inconsistent: {stats!r}")
            check(agent["tools_stored"] == 1 and
                  agent["tools_running"] == 1,
                  f"agent tool counters are inconsistent: {stats!r}")
            check(agent["logical_bytes"] > 0,
                  f"agent logical byte counter did not advance: {stats!r}")
            check(agent["lease_acquires"] >= 1,
                  f"agent lease counter did not advance: {stats!r}")
            check(kv_server.terminate_group(signal.SIGTERM) == 0,
                  f"KV agent stats shutdown failed:\n{kv_server.log}")
        finally:
            kv_server.cleanup()


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
    "ShutdownFailureExit": test_shutdown_failure_exit_code,
    "HttpSyncCrashRecovery": test_http_sync_crash_recovery,
    "KvSyncCrashRecovery": test_kv_sync_crash_recovery,
    "IntervalFlushRecovery": test_interval_flush_recovery,
    "BackgroundCheckpointRenameCrash": test_background_checkpoint_rename_crash,
    "WriterLockConflict": test_writer_lock_conflict,
    "WalTailTruncation": test_wal_tail_truncation,
    "ManifestCorruption": test_manifest_corruption,
    "MissingWalSegment": test_missing_wal_segment,
    "WalMiddleCorruption": test_wal_middle_corruption,
    "LegacyImport": test_legacy_import,
    "RandomKillSeed": test_random_kill_seed,
    "TtlCrashRecovery": test_ttl_crash_recovery,
    "StatsContract": test_stats_contract,
    "TransactionContract": test_transaction_contract,
    "AgentIdempotencyCrashRecovery": test_agent_idempotency_crash_recovery,
    "AgentLeaseTakeover": test_agent_lease_takeover,
    "AgentCrashRecovery": test_agent_crash_recovery,
    "AgentNamespaceCollision": test_agent_namespace_collision,
    "AgentStatsContract": test_agent_stats_contract,
    "KvAsyncProtocol": test_kv_async_protocol,
    "KvDisconnectSafety": test_kv_disconnect_safety,
    "HttpAsyncProtocol": test_http_async_protocol,
    "HttpDisconnectSafety": test_http_disconnect_safety,
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
