#!/usr/bin/env python3
"""Exercise the Stage B echo experiments and preserve raw evidence.

The build directory must already have been configured by the top-level
CMakeLists.  This tool builds only the two experiment targets, then runs the
blocking subset and the full non-blocking matrix.  It never deletes or reuses
an evidence directory.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import time


STARTUP_TIMEOUT_SECONDS = 10.0
CLIENT_TIMEOUT_SECONDS = 240.0
FD_SETTLE_TIMEOUT_SECONDS = 10.0


class AcceptanceError(RuntimeError):
    pass


def run(command, cwd, log_path, env=None, timeout=CLIENT_TIMEOUT_SECONDS):
    environment = os.environ.copy()
    if env:
        environment.update(env)
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write("$ " + " ".join(command) + "\n")
        log.flush()
        process = subprocess.run(
            command,
            cwd=cwd,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        log.write(f"\nEXIT_CODE={process.returncode}\n")
    return process.returncode


def read_text(path):
    return path.read_text(encoding="utf-8", errors="replace")


def wait_for_port(log_path, process, deadline):
    pattern = re.compile(r"listening on port (\d+)", re.IGNORECASE)
    while time.monotonic() < deadline:
        if log_path.is_file():
            match = pattern.search(read_text(log_path))
            if match:
                return int(match.group(1))
        if process.poll() is not None:
            raise AcceptanceError(
                f"server exited before advertising a port:\n{read_text(log_path)}")
        time.sleep(0.02)
    raise AcceptanceError("timed out waiting for the echo server port")


def fd_count(pid):
    fd_dir = Path(f"/proc/{pid}/fd")
    try:
        return len(list(fd_dir.iterdir()))
    except FileNotFoundError as error:
        raise AcceptanceError("server exited while counting file descriptors") from error


def wait_for_fd_baseline(pid, baseline, deadline):
    observed = fd_count(pid)
    while observed != baseline and time.monotonic() < deadline:
        time.sleep(0.02)
        observed = fd_count(pid)
    return observed


def stop_server(process, graceful):
    if process.poll() is not None:
        return process.returncode
    process.send_signal(signal.SIGTERM)
    try:
        return process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.wait(timeout=5)


def sanitizer_findings(server_log):
    text = read_text(server_log)
    markers = (
        "AddressSanitizer",
        "LeakSanitizer",
        "UndefinedBehaviorSanitizer",
        "runtime error:",
        "ThreadSanitizer",
    )
    return [marker for marker in markers if marker in text]


def run_server_case(*, source, server_binary, mode, test_name, evidence,
                    env, use_strace):
    server_log = evidence / f"{mode}-server.log"
    client_log = evidence / f"{mode}-client.log"
    strace_log = evidence / f"{mode}-strace.log"
    command = [str(server_binary)]
    if mode == "nonblocking":
        command.extend(["--sndbuf", "4096"])
    if use_strace:
        command = [
            "strace", "-f", "-tt", "-T", "-o", str(strace_log),
            "-e", "trace=accept4,accept,read,sendto,shutdown,close,poll,ppoll,fcntl",
        ] + command

    with server_log.open("w", encoding="utf-8", errors="replace") as log:
        process = subprocess.Popen(
            command, cwd=source, env={**os.environ, **env},
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        port = wait_for_port(server_log, process,
                             time.monotonic() + STARTUP_TIMEOUT_SECONDS)
        baseline = fd_count(process.pid)
        (evidence / f"{mode}-fd-before.txt").write_text(
            f"pid={process.pid}\nfd_count={baseline}\n", encoding="utf-8")

        client_rc = run(
            [sys.executable, "experiments/echo_client.py",
             "--port", str(port), "--test", test_name],
            source, client_log, env=env)
        if client_rc != 0:
            raise AcceptanceError(
                f"{mode} client failed with {client_rc}; see {client_log}")

        time.sleep(0.5)
        final_count = wait_for_fd_baseline(
            process.pid, baseline,
            time.monotonic() + FD_SETTLE_TIMEOUT_SECONDS)
        (evidence / f"{mode}-fd-after.txt").write_text(
            f"pid={process.pid}\nfd_count={final_count}\n"
            f"expected_fd_count={baseline}\n", encoding="utf-8")
        if final_count != baseline:
            raise AcceptanceError(
                f"{mode} fd count settled at {final_count}, expected {baseline}")

        server_rc = stop_server(process, graceful=mode == "nonblocking")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)

    findings = sanitizer_findings(server_log)
    if findings:
        raise AcceptanceError(
            f"{mode} sanitizer output detected: {', '.join(findings)}")
    if mode == "nonblocking" and server_rc != 0:
        raise AcceptanceError(
            f"non-blocking server exit status was {server_rc}, expected 0")

    stats = {}
    if mode == "nonblocking":
        match = re.search(
            r"STATS accepted=(\d+) eagain=(\d+) short_writes=(\d+) "
            r"read_eof=(\d+) reset=(\d+)",
            read_text(server_log))
        if not match:
            raise AcceptanceError("non-blocking server did not emit STATS")
        stats = {
            "accepted": int(match.group(1)),
            "eagain": int(match.group(2)),
            "short_writes": int(match.group(3)),
            "read_eof": int(match.group(4)),
            "reset": int(match.group(5)),
        }
        if stats["eagain"] <= 0:
            raise AcceptanceError("the run did not observably hit EAGAIN")
        if stats["short_writes"] <= 0:
            raise AcceptanceError("the run did not observably short-write")
        if stats["read_eof"] <= 0:
            raise AcceptanceError("the run did not observe EOF")
        if stats["reset"] <= 0:
            raise AcceptanceError("the run did not observe an RST")

    strace_summary = {}
    if use_strace:
        trace = read_text(strace_log)
        strace_summary = {
            "eagain_lines": trace.count("EAGAIN"),
            "accept4_lines": trace.count("accept4("),
            "shutdown_lines": trace.count("shutdown("),
            "close_lines": trace.count("close("),
        }
        if strace_summary["eagain_lines"] <= 0:
            raise AcceptanceError("strace did not capture EAGAIN")

    return {
        "port": port,
        "fd_count_before": baseline,
        "fd_count_after": final_count,
        "client_exit_code": client_rc,
        "server_exit_code": server_rc,
        "stats": stats,
        "strace": strace_summary,
        "sanitizer_findings": findings,
    }


def run_strace_case(*, source, server_binary, evidence, env):
    server_log = evidence / "nonblocking-strace-server.log"
    client_log = evidence / "nonblocking-strace-client.log"
    strace_log = evidence / "nonblocking-strace.log"
    command = [
        "strace", "-f", "-tt", "-T", "-o", str(strace_log),
        "-e", "trace=accept4,accept,read,sendto,shutdown,close,poll,ppoll,fcntl",
        str(server_binary), "--sndbuf", "4096",
    ]
    with server_log.open("w", encoding="utf-8", errors="replace") as log:
        process = subprocess.Popen(
            command, cwd=source, env={**os.environ, **env},
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        port = wait_for_port(server_log, process,
                             time.monotonic() + STARTUP_TIMEOUT_SECONDS)
        client_rc = run(
            [sys.executable, "experiments/echo_client.py",
             "--port", str(port), "--test", "large_slow"],
            source, client_log, env=env)
        if client_rc != 0:
            raise AcceptanceError(
                f"strace client failed with {client_rc}; see {client_log}")
        time.sleep(0.5)
    finally:
        if process.poll() is None:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)

    trace = read_text(strace_log)
    summary = {
        "eagain_lines": trace.count("EAGAIN"),
        "accept4_lines": trace.count("accept4("),
        "sendto_lines": trace.count("sendto("),
        "shutdown_lines": trace.count("shutdown("),
        "close_lines": trace.count("close("),
    }
    if summary["eagain_lines"] <= 0:
        raise AcceptanceError("strace did not capture EAGAIN")
    if summary["accept4_lines"] <= 0:
        raise AcceptanceError("strace did not capture accept4")
    if summary["shutdown_lines"] <= 0:
        raise AcceptanceError("strace did not capture shutdown")
    return summary


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default=".")
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--no-strace", action="store_true")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    source = Path(args.source).resolve()
    build_dir = Path(args.build_dir).resolve()
    evidence = Path(args.evidence).resolve()
    if evidence.exists() and any(evidence.iterdir()):
        raise AcceptanceError(f"evidence directory is not empty: {evidence}")
    evidence.mkdir(parents=True, exist_ok=True)

    blocking = build_dir / "experiments" / "socket_echo_blocking"
    nonblocking = build_dir / "experiments" / "socket_echo_nonblocking"
    if not blocking.is_file() or not nonblocking.is_file():
        raise AcceptanceError(
            "experiment binaries are missing; configure/build the top-level project first")

    build_rc = run(
        ["cmake", "--build", str(build_dir), "--target",
         "socket_echo_blocking", "socket_echo_nonblocking", "-j2"],
        source, evidence / "build.log")
    if build_rc != 0:
        raise AcceptanceError("experiment build failed")
    py_compile_rc = run(
        [sys.executable, "-m", "py_compile", "experiments/echo_client.py"],
        source, evidence / "py-compile.log")
    if py_compile_rc != 0:
        raise AcceptanceError("echo_client.py failed py_compile")

    use_strace = not args.no_strace and shutil.which("strace") is not None
    sanitizer_env = {}
    for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS"):
        if os.environ.get(name):
            sanitizer_env[name] = os.environ[name]

    result = {
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source": str(source),
        "build_dir": str(build_dir),
        "platform": platform.platform(),
        "environment": sanitizer_env,
        "strace_available": use_strace,
        "build_exit_code": build_rc,
        "py_compile_exit_code": py_compile_rc,
        "blocking": run_server_case(
            source=source, server_binary=blocking, mode="blocking",
            test_name="blocking_all", evidence=evidence,
            env=sanitizer_env, use_strace=False),
        "nonblocking": run_server_case(
            source=source, server_binary=nonblocking, mode="nonblocking",
            test_name="all", evidence=evidence,
            env=sanitizer_env, use_strace=False),
    }
    if use_strace:
        result["strace"] = run_strace_case(
            source=source, server_binary=nonblocking, evidence=evidence,
            env=sanitizer_env)
    (evidence / "summary.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AcceptanceError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
