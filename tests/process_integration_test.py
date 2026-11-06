#!/usr/bin/env python3
"""Process-level acceptance tests for the MiniDB HTTP and KV executables.

Each case uses an ephemeral loopback port and a private temporary directory.
Server processes are started in a new session so cleanup and watchdog expiry
can terminate the whole process group.  The script is intentionally limited to
the Python standard library and is registered with CTest.
"""

import argparse
import hashlib
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback


HTTP_BIN = ""
KV_BIN = ""

HTTP_PING = 1
HTTP_SET = 2
HTTP_GET = 3
HTTP_EXPIRE = 5
HTTP_TTL = 6


class TestError(Exception):
    pass


def check(condition, message):
    if not condition:
        raise TestError(message)


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
    def __init__(self, executable, port, rdb_path=None, cwd=None):
        command = [executable, "-p", str(port)]
        if rdb_path is not None:
            command += ["-r", rdb_path]
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


def start_http(port, rdb_path, cwd):
    server = ServerProcess(HTTP_BIN, port, rdb_path, cwd)
    try:
        server.wait_for_http(port)
    except Exception:
        server.cleanup()
        raise
    return server


def start_kv(port, rdb_path, cwd):
    server = ServerProcess(KV_BIN, port, rdb_path, cwd)
    try:
        server.wait_for_kv(port)
    except Exception:
        server.cleanup()
        raise
    return server


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


def test_cli_contract():
    with tempfile.TemporaryDirectory(prefix="minidb-process-cli-") as temp_dir:
        for binary, label in ((HTTP_BIN, "HTTP"), (KV_BIN, "KV")):
            help_run = run_cli(binary, ["-h"], temp_dir)
            check(help_run.returncode == 0,
                  f"{label} -h returned {help_run.returncode}: {help_run.stdout}")
            check("Usage:" in help_run.stdout,
                  f"{label} -h omitted usage text: {help_run.stdout}")

            invalid_cases = (
                ["-p", "0"],
                ["-p", "65536"],
                ["-p", "not-a-port"],
                ["-p", "1", "-p", "2"],
                ["-r"],
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
            try:
                os.unlink(default_rdb)
            except FileNotFoundError:
                pass
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
            try:
                os.unlink(default_rdb)
            except FileNotFoundError:
                pass


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
            check("RDB saved" in server.log,
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
            check("RDB saved" in server.log,
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
            return_code = server.terminate_group(signal.SIGTERM)
            check(return_code == 1,
                  f"save failure returned {return_code}, expected 1:\n{server.log}")
            check("failed to persist" in server.log.lower(),
                  f"save failure was not reported:\n{server.log}")
        finally:
            os.chmod(temp_dir, original_mode)
            server.cleanup()


CASES = {
    "CliContract": test_cli_contract,
    "HttpPersistence": test_http_persistence_and_sigterm,
    "KvPersistence": test_kv_persistence_and_sigint,
    "CorruptRdbStartup": test_corrupt_rdb_startup_fails_without_overwrite,
    "SaveFailureExit": test_save_failure_exit_code,
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
