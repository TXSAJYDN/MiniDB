#!/usr/bin/env python3
"""Measure batch-formation opportunity around real SYNC WAL fsync windows.

This is an external, non-production experiment.  It starts the unmodified KV
server under strace, drives a fixed 32-client closed-loop SET workload, and
correlates client-observed request timestamps with successful WAL data fsync
windows.  It does not add a writer, queue, batch, completion, or durability
mode to the server.

The primary metric is deliberately conservative: a request contributes to a
window only when it was fully sent before fsync entry and was still
unacknowledged at fsync exit.  The data demonstrates an opportunity for batch
formation; it is not proof that a group-commit implementation exists or that a
future implementation will meet its durability and resource bounds.
"""

import argparse
import datetime
import hashlib
import json
import os
import platform
import random
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

from benchmark import (  # noqa: E402
    free_loopback_port,
    kv_encode,
    kv_read_frame,
    latency_summary,
)

KV_PING = 1
KV_SET = 2
KV_STATS = 8
KV_OK = 101
KV_VALUE = 102

SCHEMA = "minidb-r4a-batch-formation-v1"
DEFAULT_OUTPUT = "bench/results/r4a-batch-formation-20261111.json"

G3_POLICY = {
    "rounds": 5,
    "clients": 32,
    "duration_seconds": 2.0,
    "dataset_keys": 128,
    "value_size": 64,
    "seed": 20261111,
    "minimum_successes_per_round": 400,
    "minimum_windows_per_round": 200,
    "minimum_round_p50": 8,
    "minimum_round_p90": 8,
    "minimum_round_ge_8_fraction": 0.80,
    "minimum_pooled_ge_8_fraction": 0.90,
    "maximum_errors": 0,
}

PENDING_BINS = (
    ("0", 0, 0),
    ("1", 1, 1),
    ("2-3", 2, 3),
    ("4-7", 4, 7),
    ("8-15", 8, 15),
    ("16-23", 16, 23),
    ("24-31", 24, 31),
    ("32+", 32, None),
)

TRACE_START_RE = re.compile(
    r"^(?P<pid>\d+)\s+(?P<timestamp>\d+\.\d+)\s+fsync\(")
TRACE_RETURN_RE = re.compile(
    r"\)\s+=\s+(?P<return>-?\d+)(?:\s+<(?P<duration>\d+\.\d+)>)?\s*$")
TRACE_FD_RE = re.compile(r"fsync\(\s*\d+<(?P<path>[^>]*)>")
WAL_FILE_RE = re.compile(r"(?:^|/)wal-\d{20}\.log$")


def realtime_ns():
    return time.clock_gettime_ns(time.CLOCK_REALTIME)


def decimal_seconds_to_ns(text):
    """Convert a non-negative or negative decimal seconds string to nanoseconds."""
    sign = 1
    if text.startswith("-"):
        sign = -1
        text = text[1:]
    elif text.startswith("+"):
        text = text[1:]
    whole, separator, fraction = text.partition(".")
    if not whole:
        raise ValueError("missing whole seconds in " + repr(text))
    if not separator:
        fraction = "0"
    if not fraction:
        fraction = "0"
    nanoseconds = int(whole) * 1_000_000_000 + int(
        (fraction + "000000000")[:9])
    return sign * nanoseconds


def _finish_trace_record(pid, timestamp_text, parts):
    text = "\n".join(parts)
    match = TRACE_RETURN_RE.search(text)
    if match is None:
        return None
    duration_ns = None
    if match.group("duration") is not None:
        duration_ns = decimal_seconds_to_ns(match.group("duration"))
    fd_match = TRACE_FD_RE.search(text)
    path = fd_match.group("path") if fd_match is not None else None
    start_ns = decimal_seconds_to_ns(timestamp_text)
    return {
        "pid": pid,
        "start_ns": start_ns,
        "duration_ns": duration_ns,
        "end_ns": None if duration_ns is None else start_ns + duration_ns,
        "return_code": int(match.group("return")),
        "path": path,
        "is_wal_data": bool(path is not None and WAL_FILE_RE.search(path)),
    }


def parse_fsync_trace(text):
    """Parse complete strace fsync records, including wrapped fd paths."""
    records = []
    pending = None
    line_count = 0
    for line in text.splitlines():
        line_count += 1
        start_match = TRACE_START_RE.match(line)
        if start_match is not None:
            if pending is not None:
                raise ValueError(
                    "encountered a new fsync record before the previous one "
                    "completed")
            pending = {
                "pid": int(start_match.group("pid")),
                "timestamp": start_match.group("timestamp"),
                "parts": [line],
            }
        elif pending is not None:
            pending["parts"].append(line)

        if pending is not None:
            record = _finish_trace_record(
                pending["pid"], pending["timestamp"], pending["parts"])
            if record is not None:
                records.append(record)
                pending = None

    if pending is not None:
        raise ValueError("trace ended with an incomplete fsync record")
    return records, line_count


def pending_histogram(values):
    rows = []
    for label, lower, upper in PENDING_BINS:
        if upper is None:
            count = sum(1 for value in values if value >= lower)
        else:
            count = sum(1 for value in values if lower <= value <= upper)
        rows.append({
            "label": label,
            "minimum": lower,
            "maximum": upper,
            "count": count,
        })
    return rows


def connect_kv(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    sock.settimeout(10.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def send_ping(sock):
    sock.sendall(kv_encode(KV_PING, []))
    command_type, _ = kv_read_frame(sock)
    if command_type != KV_OK:
        raise RuntimeError("PING returned " + str(command_type))


def read_stats(port):
    with connect_kv(port) as sock:
        sock.sendall(kv_encode(KV_STATS, []))
        command_type, args = kv_read_frame(sock)
    if command_type != KV_VALUE or len(args) != 1:
        raise RuntimeError(
            "STATS returned type=" + str(command_type) +
            ", args=" + repr(args))
    return json.loads(args[0].decode("utf-8"))


def make_dataset(key_count, value_size):
    return [
        (("r4a-key-" + str(index).zfill(4)),
         bytes(((index * 29 + offset * 17) & 0xff
                for offset in range(value_size))))
        for index in range(key_count)
    ]


def preload_dataset(port, dataset):
    sock = connect_kv(port)
    try:
        for key, value in dataset:
            sock.sendall(kv_encode(KV_SET, [key, value]))
            command_type, _ = kv_read_frame(sock)
            if command_type != KV_OK:
                raise RuntimeError(
                    "dataset preload failed for " + key + ": " +
                    str(command_type))
    finally:
        sock.close()


def worker(client_index, port, encoded_requests, duration_seconds, seed,
           ready_barrier, start_event, deadline_box):
    rng = random.Random(seed + client_index * 1000003)
    result = {
        "attempts": 0,
        "successes": 0,
        "errors": 0,
        "requests": [],
        "error_samples": [],
    }
    sock = None
    reached_barrier = False
    try:
        sock = connect_kv(port)
        send_ping(sock)
        ready_barrier.wait(timeout=30.0)
        reached_barrier = True
        if not start_event.wait(timeout=30.0):
            raise RuntimeError("measurement start event timed out")
        deadline_ns = deadline_box["monotonic_ns"]
        while time.monotonic_ns() < deadline_ns:
            payload = encoded_requests[rng.randrange(len(encoded_requests))]
            started_ns = realtime_ns()
            result["attempts"] += 1
            sock.sendall(payload)
            send_completed_ns = realtime_ns()
            command_type, _ = kv_read_frame(sock)
            acknowledged_ns = realtime_ns()
            if command_type == KV_OK:
                result["successes"] += 1
                result["requests"].append([
                    started_ns,
                    send_completed_ns,
                    acknowledged_ns,
                ])
            else:
                result["errors"] += 1
                if len(result["error_samples"]) < 5:
                    result["error_samples"].append(
                        "unexpected SET response " + str(command_type))
                break
    except Exception as error:
        result["errors"] += 1
        if len(result["error_samples"]) < 5:
            result["error_samples"].append(
                type(error).__name__ + ": " + str(error))
        if not reached_barrier:
            try:
                ready_barrier.abort()
            except Exception:
                pass
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
    return result


class TracedServer:
    def __init__(self, strace_binary, kv_binary, port, rdb_path, trace_path,
                 log_path):
        self.port = port
        self.trace_path = Path(trace_path)
        self.log_path = Path(log_path)
        self.log_file = open(self.log_path, "wb")
        command = [
            strace_binary,
            "-f",
            "-ttt",
            "-T",
            "-yy",
            "-qq",
            "-e",
            "trace=fsync",
            "-o",
            str(self.trace_path),
            "--",
            str(kv_binary),
            "-p",
            str(port),
            "-r",
            str(rdb_path),
            "-d",
            "sync",
        ]
        self.command = command
        self.proc = subprocess.Popen(
            command,
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.server_pid = None

    def _direct_children(self):
        children_path = Path(
            "/proc/" + str(self.proc.pid) + "/task/" +
            str(self.proc.pid) + "/children")
        try:
            text = children_path.read_text(encoding="ascii").strip()
        except OSError:
            return []
        if not text:
            return []
        return [int(item) for item in text.split()]

    def wait_ready(self):
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    "traced server exited with " + str(self.proc.returncode) +
                    " during startup; see " + str(self.log_path))
            children = self._direct_children()
            if children:
                self.server_pid = children[0]
            try:
                with connect_kv(self.port) as sock:
                    sock.sendall(kv_encode(KV_PING, []))
                    if kv_read_frame(sock)[0] == KV_OK:
                        return
            except (OSError, RuntimeError, ValueError):
                pass
            time.sleep(0.02)
        raise RuntimeError("server did not become ready within 10 seconds")

    def stop(self):
        child_pid = self.server_pid
        if child_pid is None:
            children = self._direct_children()
            child_pid = children[0] if children else None
        if self.proc.poll() is None:
            if child_pid is not None and Path(
                    "/proc/" + str(child_pid)).exists():
                try:
                    os.kill(child_pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            else:
                try:
                    os.killpg(self.proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            try:
                self.proc.wait(timeout=15.0)
            except subprocess.TimeoutExpired:
                if child_pid is not None:
                    try:
                        os.kill(child_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.proc.wait(timeout=5.0)
        self.log_file.close()
        return self.proc.returncode


def percentile(sorted_values, fraction):
    if not sorted_values:
        return 0.0
    position = fraction * (len(sorted_values) - 1)
    lower = int(position)
    upper = min(lower + 1, len(sorted_values) - 1)
    weight = position - lower
    return (sorted_values[lower] * (1.0 - weight) +
            sorted_values[upper] * weight)


def summarize_numeric(values, scale=1.0):
    if not values:
        return {
            "count": 0,
            "min": 0.0,
            "mean": 0.0,
            "p50": 0.0,
            "p90": 0.0,
            "p99": 0.0,
            "max": 0.0,
        }
    ordered = sorted(value * scale for value in values)
    return {
        "count": len(ordered),
        "min": round(ordered[0], 3),
        "mean": round(statistics.fmean(ordered), 3),
        "p50": round(percentile(ordered, 0.50), 3),
        "p90": round(percentile(ordered, 0.90), 3),
        "p99": round(percentile(ordered, 0.99), 3),
        "max": round(ordered[-1], 3),
    }


def summarize_pending(values):
    summary = summarize_numeric(values, scale=1.0)
    ge_8_count = sum(1 for value in values if value >= 8)
    summary.update({
        "ge_8_count": ge_8_count,
        "ge_8_fraction": (
            round(ge_8_count / len(values), 6) if values else 0.0),
        "histogram": pending_histogram(values),
    })
    return summary


def analyze_windows(parsed_fsyncs, requests, measurement_start_ns,
                    measurement_end_ns):
    failed_wal_fsyncs = 0
    unresolved_wal_fsyncs = 0
    windows = []
    for record in parsed_fsyncs:
        if not record["is_wal_data"]:
            continue
        if (record["start_ns"] < measurement_start_ns or
                record["start_ns"] > measurement_end_ns):
            continue
        if record["duration_ns"] is None or record["end_ns"] is None:
            unresolved_wal_fsyncs += 1
            continue
        if record["return_code"] != 0:
            failed_wal_fsyncs += 1
            continue
        windows.append(record)

    window_rows = []
    pending_values = []
    fsync_durations_us = []
    for record in windows:
        fsync_start_ns = record["start_ns"]
        fsync_end_ns = record["end_ns"]
        pending_count = 0
        pending_at_start_count = 0
        pending_at_end_count = 0
        arrivals_during_count = 0
        overlap_count = 0
        for request in requests:
            send_completed_ns = request[1]
            acknowledged_ns = request[2]
            if (send_completed_ns <= fsync_start_ns and
                    acknowledged_ns >= fsync_end_ns):
                pending_count += 1
            if (send_completed_ns <= fsync_start_ns and
                    acknowledged_ns > fsync_start_ns):
                pending_at_start_count += 1
            if (send_completed_ns <= fsync_end_ns and
                    acknowledged_ns > fsync_end_ns):
                pending_at_end_count += 1
            if (fsync_start_ns < send_completed_ns <= fsync_end_ns and
                    acknowledged_ns > fsync_end_ns):
                arrivals_during_count += 1
            if (send_completed_ns <= fsync_end_ns and
                    acknowledged_ns >= fsync_start_ns):
                overlap_count += 1

        pending_values.append(pending_count)
        fsync_duration_us = record["duration_ns"] / 1000.0
        fsync_durations_us.append(fsync_duration_us)
        window_rows.append({
            "pid": record["pid"],
            "start_ns": fsync_start_ns,
            "end_ns": fsync_end_ns,
            "duration_us": round(fsync_duration_us, 3),
            "pending_count": pending_count,
            "pending_at_start_count": pending_at_start_count,
            "pending_at_end_count": pending_at_end_count,
            "arrivals_during_count": arrivals_during_count,
            "overlap_count": overlap_count,
        })

    client_latencies_us = [
        (request[2] - request[0]) / 1000.0 for request in requests
    ]
    return {
        "measurement_start_ns": measurement_start_ns,
        "measurement_end_ns": measurement_end_ns,
        "request_count": len(requests),
        "window_count": len(window_rows),
        "failed_wal_data_fsyncs": failed_wal_fsyncs,
        "unresolved_wal_data_fsyncs": unresolved_wal_fsyncs,
        "pending": summarize_pending(pending_values),
        "fsync_duration_us": summarize_numeric(fsync_durations_us),
        "client_latency_us": latency_summary(client_latencies_us),
        "windows": window_rows,
    }


def _delta(stats_after, stats_before, group, field):
    before = stats_before[group][field]
    after = stats_after[group][field]
    if (type(before) is not int or type(after) is not int or after < before):
        raise RuntimeError(
            "STATS counter is not monotonic: " + group + "." + field +
            " " + repr(before) + " -> " + repr(after))
    return after - before


def counter_deltas(stats_after, stats_before):
    return {
        "wal_appends": _delta(stats_after, stats_before, "wal", "appends"),
        "wal_data_fsyncs": _delta(
            stats_after, stats_before, "wal", "data_fsyncs"),
        "sync_mutations": _delta(
            stats_after, stats_before, "persistence", "sync_mutations"),
        "wal_bytes_written": _delta(
            stats_after, stats_before, "wal", "bytes_written"),
    }


def command_output(command, cwd=None):
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10.0,
            check=False,
        )
        return result.stdout.splitlines()[0] if result.stdout else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def cpu_model():
    try:
        for line in Path("/proc/cpuinfo").read_text(
                encoding="utf-8", errors="replace").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_status(repo_root):
    result = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError("git status failed: " + result.stderr.strip())
    return result.stdout


def collect_provenance(kv_binary, strace_binary):
    repo_root = Path(__file__).resolve().parent.parent
    status = git_status(repo_root)
    git_head = command_output(["git", "rev-parse", "HEAD"], cwd=repo_root)
    branch = command_output(
        ["git", "branch", "--show-current"], cwd=repo_root)
    kernel_release = platform.release()
    try:
        kernel_version = Path("/proc/version").read_text(
            encoding="utf-8", errors="replace").strip()
    except OSError:
        kernel_version = ""
    harness_path = Path(__file__).resolve()
    return {
        "schema": SCHEMA,
        "git_head": git_head,
        "git_branch": branch,
        "working_tree_dirty_at_collection": bool(status.strip()),
        "working_tree_status": status.splitlines(),
        "harness_path": str(harness_path),
        "harness_sha256": sha256_file(harness_path),
        "kv_binary_path": str(Path(kv_binary).resolve()),
        "kv_binary_sha256": sha256_file(kv_binary),
        "strace_path": str(Path(strace_binary).resolve()),
        "strace_version": command_output([strace_binary, "--version"]),
        "python_version": sys.version.split()[0],
        "platform": platform.platform(),
        "kernel_release": kernel_release,
        "kernel_version": kernel_version,
        "is_wsl": "microsoft" in kernel_version.lower(),
        "cpu_model": cpu_model(),
        "logical_cpu_count": os.cpu_count(),
    }


def aggregate_rounds(rounds, policy):
    pending_values = []
    fsync_durations_us = []
    client_latencies_us = []
    for item in rounds:
        pending_values.extend(
            window["pending_count"] for window in item["analysis"]["windows"])
        fsync_durations_us.extend(
            window["duration_us"] for window in item["analysis"]["windows"])
        requests = item["requests"]
        client_latencies_us.extend(
            (request[2] - request[0]) / 1000.0 for request in requests)

    ge_8_count = sum(1 for value in pending_values if value >= 8)
    pooled_ge_8_fraction = (
        round(ge_8_count / len(pending_values), 6)
        if pending_values else 0.0)
    return {
        "round_count": len(rounds),
        "attempts": sum(item["attempts"] for item in rounds),
        "successes": sum(item["successes"] for item in rounds),
        "errors": sum(item["errors"] for item in rounds),
        "window_count": len(pending_values),
        "pending": summarize_pending(pending_values),
        "pooled_ge_8_fraction": pooled_ge_8_fraction,
        "fsync_duration_us": summarize_numeric(fsync_durations_us),
        "client_latency_us": latency_summary(client_latencies_us),
        "throughput_ops_per_second_median": round(statistics.median(
            [item["throughput_ops_per_second"] for item in rounds]), 3),
        "per_round": [
            {
                "round": item["round"],
                "successes": item["successes"],
                "errors": item["errors"],
                "window_count": item["analysis"]["window_count"],
                "pending_p50": item["analysis"]["pending"]["p50"],
                "pending_p90": item["analysis"]["pending"]["p90"],
                "pending_ge_8_fraction":
                    item["analysis"]["pending"]["ge_8_fraction"],
                "throughput_ops_per_second":
                    item["throughput_ops_per_second"],
            }
            for item in rounds
        ],
    }


def _gate_check(name, passed, observed, requirement):
    return {
        "name": name,
        "pass": bool(passed),
        "observed": observed,
        "requirement": requirement,
    }


def evaluate_g3(rounds, policy, protocol, aggregate):
    checks = []
    checks.append(_gate_check(
        "frozen_protocol",
        len(rounds) == policy["rounds"] and
        protocol["clients"] == policy["clients"] and
        protocol["duration_seconds"] == policy["duration_seconds"] and
        protocol["dataset_keys"] == policy["dataset_keys"] and
        protocol["value_size"] == policy["value_size"] and
        protocol["seed"] == policy["seed"],
        {
            "rounds": len(rounds),
            "clients": protocol["clients"],
            "duration_seconds": protocol["duration_seconds"],
            "dataset_keys": protocol["dataset_keys"],
            "value_size": protocol["value_size"],
            "seed": protocol["seed"],
        },
        {
            "rounds": policy["rounds"],
            "clients": policy["clients"],
            "duration_seconds": policy["duration_seconds"],
            "dataset_keys": policy["dataset_keys"],
            "value_size": policy["value_size"],
            "seed": policy["seed"],
        },
    ))
    checks.append(_gate_check(
        "successes_per_round",
        all(item["successes"] >= policy["minimum_successes_per_round"]
            for item in rounds),
        [item["successes"] for item in rounds],
        "every round >= " + str(policy["minimum_successes_per_round"]),
    ))
    checks.append(_gate_check(
        "windows_per_round",
        all(item["analysis"]["window_count"] >=
            policy["minimum_windows_per_round"] for item in rounds),
        [item["analysis"]["window_count"] for item in rounds],
        "every round >= " + str(policy["minimum_windows_per_round"]),
    ))
    checks.append(_gate_check(
        "total_errors",
        aggregate["errors"] <= policy["maximum_errors"],
        aggregate["errors"],
        "<= " + str(policy["maximum_errors"]),
    ))
    counter_rows = []
    counter_ok = True
    for item in rounds:
        counters = item["counters"]
        row = {
            "round": item["round"],
            "successes": item["successes"],
            "wal_appends": counters["wal_appends"],
            "wal_data_fsyncs": counters["wal_data_fsyncs"],
            "sync_mutations": counters["sync_mutations"],
            "trace_wal_data_fsyncs": item["analysis"]["window_count"],
        }
        counter_rows.append(row)
        if not (item["successes"] == counters["wal_appends"] ==
                counters["wal_data_fsyncs"] ==
                counters["sync_mutations"] ==
                item["analysis"]["window_count"]):
            counter_ok = False
    checks.append(_gate_check(
        "counter_and_trace_invariants",
        counter_ok and
        all(item["analysis"]["failed_wal_data_fsyncs"] == 0 and
            item["analysis"]["unresolved_wal_data_fsyncs"] == 0
            for item in rounds),
        counter_rows,
        "successes == WAL appends == WAL data fsyncs == SYNC mutations == "
        "trace windows; no failed or unresolved WAL fsyncs",
    ))
    checks.append(_gate_check(
        "round_pending_p50",
        all(item["analysis"]["pending"]["p50"] >=
            policy["minimum_round_p50"] for item in rounds),
        [item["analysis"]["pending"]["p50"] for item in rounds],
        "every round >= " + str(policy["minimum_round_p50"]),
    ))
    checks.append(_gate_check(
        "round_pending_p90",
        all(item["analysis"]["pending"]["p90"] >=
            policy["minimum_round_p90"] for item in rounds),
        [item["analysis"]["pending"]["p90"] for item in rounds],
        "every round >= " + str(policy["minimum_round_p90"]),
    ))
    checks.append(_gate_check(
        "round_ge_8_fraction",
        all(item["analysis"]["pending"]["ge_8_fraction"] >=
            policy["minimum_round_ge_8_fraction"] for item in rounds),
        [item["analysis"]["pending"]["ge_8_fraction"] for item in rounds],
        "every round >= " +
        str(policy["minimum_round_ge_8_fraction"]),
    ))
    checks.append(_gate_check(
        "pooled_ge_8_fraction",
        aggregate["pooled_ge_8_fraction"] >=
        policy["minimum_pooled_ge_8_fraction"],
        aggregate["pooled_ge_8_fraction"],
        ">= " + str(policy["minimum_pooled_ge_8_fraction"]),
    ))
    return {
        "verdict": "PASS" if all(check["pass"] for check in checks)
        else "FAIL",
        "checks": checks,
    }


def run_round(args, round_index):
    temp_dir = tempfile.TemporaryDirectory(
        prefix="minidb-r4a-batch-" + str(round_index) + "-",
        dir=args.temp_dir,
    )
    server = None
    server_stopped = False
    try:
        work_dir = Path(temp_dir.name)
        rdb_path = work_dir / "db.rdb"
        trace_path = work_dir / "fsync.trace"
        log_path = work_dir / "server.log"
        port = free_loopback_port()
        server = TracedServer(
            args.strace,
            args.kv,
            port,
            rdb_path,
            trace_path,
            log_path,
        )
        server.wait_ready()

        dataset = make_dataset(args.dataset_keys, args.value_size)
        encoded_requests = [
            kv_encode(KV_SET, [key, value]) for key, value in dataset
        ]
        preload_dataset(port, dataset)
        stats_before = read_stats(port)

        ready_barrier = threading.Barrier(args.clients + 1)
        start_event = threading.Event()
        deadline_box = {}
        results = {}
        threads = []
        for client_index in range(args.clients):
            thread = threading.Thread(
                target=lambda index=client_index: results.__setitem__(
                    index,
                    worker(
                        index,
                        port,
                        encoded_requests,
                        args.duration_seconds,
                        args.seed,
                        ready_barrier,
                        start_event,
                        deadline_box,
                    ),
                ),
                name="r4a-client-" + str(client_index),
                daemon=True,
            )
            threads.append(thread)
            thread.start()

        try:
            ready_barrier.wait(timeout=30.0)
        except threading.BrokenBarrierError as error:
            raise RuntimeError(
                "client readiness barrier failed; a worker could not connect"
            ) from error

        measurement_start_monotonic_ns = time.monotonic_ns()
        measurement_start_ns = realtime_ns()
        deadline_box["monotonic_ns"] = (
            measurement_start_monotonic_ns +
            int(args.duration_seconds * 1_000_000_000))
        start_event.set()

        join_timeout = args.duration_seconds + 30.0
        for thread in threads:
            thread.join(timeout=join_timeout)
        alive_threads = [thread.name for thread in threads if thread.is_alive()]
        if alive_threads:
            raise RuntimeError(
                "workers did not stop after the measurement deadline: " +
                ", ".join(alive_threads))

        measurement_end_monotonic_ns = time.monotonic_ns()
        measurement_end_ns = realtime_ns()
        stats_after = read_stats(port)
        elapsed_seconds = (
            measurement_end_monotonic_ns - measurement_start_monotonic_ns
        ) / 1_000_000_000.0

        server_return_code = server.stop()
        server_stopped = True

        if server_return_code != 0:
            raise RuntimeError(
                "traced server shutdown returned " +
                str(server_return_code) + "; see " + str(log_path))

        trace_bytes = trace_path.stat().st_size
        trace_sha256 = sha256_file(trace_path)
        trace_text = trace_path.read_text(encoding="utf-8", errors="replace")
        parsed_fsyncs, trace_line_count = parse_fsync_trace(trace_text)
        wal_data_fsyncs_all = sum(
            1 for record in parsed_fsyncs if record["is_wal_data"])

        requests = []
        attempts = 0
        successes = 0
        errors = 0
        error_samples = []
        for client_index in range(args.clients):
            result = results.get(client_index)
            if result is None:
                raise RuntimeError(
                    "client " + str(client_index) + " returned no result")
            attempts += result["attempts"]
            successes += result["successes"]
            errors += result["errors"]
            requests.extend(result["requests"])
            error_samples.extend(result["error_samples"][:5])
        requests.sort(key=lambda item: item[0])

        analysis = analyze_windows(
            parsed_fsyncs,
            requests,
            measurement_start_ns,
            measurement_end_ns,
        )
        counters = counter_deltas(stats_after, stats_before)
        return {
            "round": round_index,
            "attempts": attempts,
            "successes": successes,
            "errors": errors,
            "error_samples": error_samples[:5],
            "elapsed_seconds": round(elapsed_seconds, 6),
            "throughput_ops_per_second": (
                round(successes / elapsed_seconds, 3)
                if elapsed_seconds > 0 else 0.0),
            "measurement_start_ns": measurement_start_ns,
            "measurement_end_ns": measurement_end_ns,
            "counters": counters,
            "analysis": analysis,
            "requests": requests,
            "trace": {
                "trace_file_bytes": trace_bytes,
                "trace_file_sha256": trace_sha256,
                "trace_line_count": trace_line_count,
                "parsed_fsync_count": len(parsed_fsyncs),
                "wal_data_fsync_count_all_positions": wal_data_fsyncs_all,
            },
            "server_return_code": server_return_code,
        }
    finally:
        if server is not None and not server_stopped:
            try:
                server.stop()
            except Exception:
                pass
        temp_dir.cleanup()


def make_fake_round(round_number, p50, p90, ge_fraction, successes=500):
    return {
        "round": round_number,
        "attempts": successes,
        "successes": successes,
        "errors": 0,
        "throughput_ops_per_second": 625.0,
        "requests": [],
        "counters": {
            "wal_appends": successes,
            "wal_data_fsyncs": successes,
            "sync_mutations": successes,
            "wal_bytes_written": successes * 100,
        },
        "analysis": {
            "window_count": successes,
            "failed_wal_data_fsyncs": 0,
            "unresolved_wal_data_fsyncs": 0,
            "pending": {
                "p50": p50,
                "p90": p90,
                "ge_8_fraction": ge_fraction,
            },
            "windows": [],
        },
    }


def run_self_tests():
    trace_text = (
        "1234 1794372014.507911 fsync(\n"
        "  10</tmp/db.rdb.wal/wal-00000000000000000001.log>\n"
        ") = 0 <0.001528>\n"
        "1234 1794372014.510000 fsync("
        "11</tmp/db.rdb.wal>) = 0 <0.000123>\n"
        "1234 1794372014.520000 fsync("
        "12</tmp/db.rdb.tmp>) = 0 <0.000050>\n"
    )
    records, line_count = parse_fsync_trace(trace_text)
    assert line_count == 5
    assert len(records) == 3
    wal_records = [record for record in records if record["is_wal_data"]]
    assert len(wal_records) == 1
    assert wal_records[0]["duration_ns"] == 1_528_000
    assert wal_records[0]["end_ns"] == (
        wal_records[0]["start_ns"] + 1_528_000)

    fake_window = {
        "pid": 1,
        "start_ns": 1000,
        "end_ns": 2000,
        "duration_ns": 1000,
        "return_code": 0,
        "path": "/tmp/db.rdb.wal/wal-00000000000000000001.log",
        "is_wal_data": True,
    }
    fake_requests = [
        [0, 900, 2500],
        [0, 1000, 2500],
        [0, 1500, 2500],
        [0, 900, 1500],
        [0, 2500, 3000],
        [0, 900, 2000],
    ]
    analysis = analyze_windows(
        [fake_window], fake_requests, 0, 3000)
    assert analysis["window_count"] == 1
    window = analysis["windows"][0]
    assert window["pending_count"] == 3
    assert window["pending_at_start_count"] == 4
    assert window["pending_at_end_count"] == 3
    assert window["arrivals_during_count"] == 1
    assert window["overlap_count"] == 5

    policy = dict(G3_POLICY)
    protocol = {
        "clients": policy["clients"],
        "duration_seconds": policy["duration_seconds"],
        "dataset_keys": policy["dataset_keys"],
        "value_size": policy["value_size"],
        "seed": policy["seed"],
    }
    passing_rounds = [
        make_fake_round(index + 1, 10, 12, 0.96)
        for index in range(policy["rounds"])
    ]
    passing_aggregate = {
        "errors": 0,
        "pooled_ge_8_fraction": 0.96,
    }
    assert evaluate_g3(
        passing_rounds, policy, protocol, passing_aggregate)["verdict"] == "PASS"

    failing_rounds = [
        make_fake_round(index + 1, 10, 12, 0.96)
        for index in range(policy["rounds"])
    ]
    failing_rounds[2]["analysis"]["pending"]["p50"] = 7
    assert evaluate_g3(
        failing_rounds, policy, protocol, passing_aggregate)["verdict"] == "FAIL"
    print("self-test: PASS")
    return 0


def parse_arguments(argv):
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Measure R4-A batch-formation opportunity without "
                    "changing the production write path.")
    parser.add_argument(
        "--kv",
        default=str(repo_root / "build/release/app/minidb_kv_server"),
        help="path to the Release minidb_kv_server binary")
    parser.add_argument(
        "--strace",
        default=shutil.which("strace") or "strace",
        help="path to strace")
    parser.add_argument(
        "--out",
        default=str(repo_root / DEFAULT_OUTPUT),
        help="new JSON artifact path; existing files are not overwritten")
    parser.add_argument(
        "--rounds", type=int, default=G3_POLICY["rounds"])
    parser.add_argument(
        "--clients", type=int, default=G3_POLICY["clients"])
    parser.add_argument(
        "--duration-seconds", type=float,
        default=G3_POLICY["duration_seconds"])
    parser.add_argument(
        "--dataset-keys", type=int, default=G3_POLICY["dataset_keys"])
    parser.add_argument(
        "--value-size", type=int, default=G3_POLICY["value_size"])
    parser.add_argument(
        "--seed", type=int, default=G3_POLICY["seed"])
    parser.add_argument(
        "--temp-dir", default=None,
        help="optional parent directory for per-round temporary data")
    parser.add_argument(
        "--self-test", action="store_true",
        help="run parser, correlation, and gate tests without a server")
    return parser, parser.parse_args(argv)


def validate_arguments(args):
    if not Path(args.kv).is_file():
        raise RuntimeError("KV server binary does not exist: " + args.kv)
    if not os.access(args.kv, os.X_OK):
        raise RuntimeError("KV server binary is not executable: " + args.kv)
    if shutil.which(args.strace) is None and not Path(args.strace).is_file():
        raise RuntimeError("strace does not exist: " + args.strace)
    if args.rounds <= 0:
        raise RuntimeError("--rounds must be positive")
    if args.clients <= 0 or args.clients > 32:
        raise RuntimeError("--clients must be in 1..32")
    if args.duration_seconds <= 0:
        raise RuntimeError("--duration-seconds must be positive")
    if args.dataset_keys <= 0:
        raise RuntimeError("--dataset-keys must be positive")
    if args.value_size <= 0:
        raise RuntimeError("--value-size must be positive")


def main(argv=None):
    parser, args = parse_arguments(argv)
    if args.self_test:
        return run_self_tests()
    validate_arguments(args)

    repo_root = Path(__file__).resolve().parent.parent
    destination = Path(args.out)
    if destination.exists():
        parser.error("refusing to overwrite existing artifact: " + str(
            destination))
    if not destination.parent.is_dir():
        parser.error("artifact parent directory does not exist: " + str(
            destination.parent))

    provenance = collect_provenance(args.kv, args.strace)
    if provenance["working_tree_dirty_at_collection"]:
        raise RuntimeError(
            "formal R4-A collection requires a clean worktree; git status:\n" +
            "\n".join(provenance["working_tree_status"]))

    protocol = {
        "durability_mode": "sync",
        "clients": args.clients,
        "duration_seconds": args.duration_seconds,
        "dataset_keys": args.dataset_keys,
        "value_size": args.value_size,
        "seed": args.seed,
        "connection_model":
            "closed-loop persistent KV connections, one in-flight SET per client",
        "trace_scope": "strace -f -ttt -T -yy, fsync syscalls only",
        "primary_metric":
            "SET fully sent at or before WAL data fsync entry and still "
            "unacknowledged at or after fsync exit",
    }

    total_started = time.monotonic()
    rounds = []
    for round_index in range(1, args.rounds + 1):
        print(
            "round " + str(round_index) + "/" + str(args.rounds) +
            ": collecting",
            flush=True,
        )
        item = run_round(args, round_index)
        rounds.append(item)
        print(json.dumps({
            "round": round_index,
            "successes": item["successes"],
            "errors": item["errors"],
            "windows": item["analysis"]["window_count"],
            "pending_p50": item["analysis"]["pending"]["p50"],
            "pending_p90": item["analysis"]["pending"]["p90"],
            "ge_8_fraction": item["analysis"]["pending"]["ge_8_fraction"],
        }), flush=True)

    status_after_collection = git_status(repo_root)
    if status_after_collection.strip():
        raise RuntimeError(
            "worktree became dirty during formal collection:\n" +
            status_after_collection)

    aggregate = aggregate_rounds(rounds, G3_POLICY)
    gate = evaluate_g3(rounds, G3_POLICY, protocol, aggregate)
    artifact = {
        "schema": SCHEMA,
        "generated_at_utc": datetime.datetime.now(
            datetime.timezone.utc).isoformat(),
        "provenance": provenance,
        "environment": {
            "platform": platform.platform(),
            "kernel_release": platform.release(),
            "is_wsl": provenance["is_wsl"],
            "cpu_model": provenance["cpu_model"],
            "logical_cpu_count": provenance["logical_cpu_count"],
            "python_version": sys.version,
            "strace_version": provenance["strace_version"],
        },
        "protocol": protocol,
        "policy": G3_POLICY,
        "method": {
            "server": "unmodified Release minidb_kv_server with -d sync",
            "trace": "strace timestamps fsync entry; -T duration gives exit",
            "clock": "CLOCK_REALTIME nanoseconds for client and strace correlation",
            "primary_pending_count":
                "count(SET send_completed_ns <= fsync_start_ns and "
                "SET acknowledged_ns >= fsync_end_ns)",
            "supplementary_counts": {
                "pending_at_start_count":
                    "fully sent before fsync entry and unacknowledged after entry",
                "pending_at_end_count":
                    "fully sent before fsync exit and unacknowledged after exit",
                "arrivals_during_count":
                    "fully sent during fsync and unacknowledged after exit",
                "overlap_count":
                    "fully sent before fsync exit and unacknowledged at/after entry",
            },
            "window_filter":
                "successful fsync to a wal-NNNNNNNNNNNNNNNNNNNN.log file with "
                "entry timestamp inside the measured client window",
            "counter_invariant":
                "measured-window STATS deltas must equal successes and parsed "
                "WAL data fsync windows",
        },
        "independence": {
            "external_observability_only": True,
            "production_write_path_changed": False,
            "group_commit_implemented": False,
            "writer_thread_implemented": False,
            "queue_implemented": False,
            "batch_implemented": False,
            "async_completion_implemented": False,
            "batched_mode_implemented": False,
        },
        "aggregate": aggregate,
        "gate": gate,
        "rounds": rounds,
        "limits": {
            "maximum_duration_seconds": args.duration_seconds,
            "client_connection_cap": 32,
            "value_size_bytes": args.value_size,
            "dataset_keys": args.dataset_keys,
        },
        "limitations": [
            "WSL2 loopback, single node, same-host client and server; not a "
            "production SLA or capacity claim.",
            "Closed-loop clients with one outstanding SET each; this measures "
            "request-level opportunity, not open-loop saturation.",
            "The pending count is an external upper-bound opportunity: it "
            "does not prove that a future server queue already contains those "
            "records or that a writer thread can batch them without overhead.",
            "Client acknowledgement timestamps include client scheduling and "
            "Python wake-up delay; they can only overstate pending duration.",
            "strace ptrace and tracing overhead affects timing and is present "
            "for every measured fsync window.",
            "The experiment does not test correctness, crash recovery, queue "
            "bounds, completion ordering, or the G4 throughput gate.",
            "Passing G3 only authorizes a separate implementation-level ADR; "
            "it does not authorize merging group commit into the write path.",
        ],
        "elapsed_wall_seconds": round(time.monotonic() - total_started, 3),
    }
    destination.write_text(
        json.dumps(artifact, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "out": str(destination),
        "verdict": gate["verdict"],
        "successes": aggregate["successes"],
        "errors": aggregate["errors"],
        "windows": aggregate["window_count"],
        "pending_p50": aggregate["pending"]["p50"],
        "pending_p90": aggregate["pending"]["p90"],
        "pooled_ge_8_fraction": aggregate["pooled_ge_8_fraction"],
        "checks_failed": [
            check["name"] for check in gate["checks"] if not check["pass"]
        ],
    }, indent=2))
    return 0 if gate["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
