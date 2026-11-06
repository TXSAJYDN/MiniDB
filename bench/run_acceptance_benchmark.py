#!/usr/bin/env python3
"""Timed, multi-round, resource-instrumented MiniDB acceptance benchmark.

This harness complements ``bench/benchmark.py``.  It keeps one persistent
loopback connection per protocol and measures a closed-loop GET workload, with
one outstanding request per connection.  Each protocol runs multiple rounds of
untimed warm-up, timed measurement, and cooling.  Percentiles are calculated
from every raw latency sample; client and server process resources are sampled
from /proc while the measured round is active.

The result is a WSL2 workstation baseline, not a saturated-capacity or
production-SLA claim.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import threading

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from benchmark import (  # noqa: E402
    KV_GET,
    KV_VALUE,
    ServerProcess,
    free_loopback_port,
    http_request,
    kv_encode,
    kv_read_frame,
    latency_summary,
    load_http_dataset,
    load_kv_dataset,
    make_dataset,
)


PROJECT_ROOT = SCRIPT_DIR.parent


def safe_latency_summary(samples_us):
    return latency_summary(samples_us) if samples_us else None


def numeric_summary(values):
    if not values:
        return None
    return {
        "min": round(min(values), 3),
        "mean": round(statistics.fmean(values), 3),
        "max": round(max(values), 3),
        "stdev": round(statistics.pstdev(values), 3),
    }


def first_line(argv):
    try:
        completed = subprocess.run(
            argv,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    output = (completed.stdout or completed.stderr).splitlines()
    return output[0].strip() if output else None


def read_os_release():
    values = {}
    try:
        for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            if "=" not in line:
                continue
            name, value = line.split("=", 1)
            values[name] = value.strip().strip('"')
    except OSError:
        pass
    return values


def read_cpu_model():
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def read_mem_total_kib():
    try:
        for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("MemTotal:"):
                return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return None


def git_output(*args):
    try:
        completed = subprocess.run(
            ["git", "-C", str(PROJECT_ROOT), *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def environment_record():
    try:
        soft_limit, hard_limit = resource.getrlimit(resource.RLIMIT_NOFILE)
    except (OSError, ValueError):
        soft_limit = hard_limit = None
    load_average = os.getloadavg() if hasattr(os, "getloadavg") else (0, 0, 0)
    os_release = read_os_release()
    return {
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "system": platform.system(),
        "kernel_release": platform.release(),
        "machine": platform.machine(),
        "os_release": {
            key: os_release.get(key)
            for key in ("PRETTY_NAME", "VERSION_ID")
            if os_release.get(key) is not None
        },
        "cpu_model": read_cpu_model(),
        "logical_cpus": os.cpu_count(),
        "memory_total_kib": read_mem_total_kib(),
        "python": platform.python_version(),
        "compiler": first_line(["g++", "--version"]),
        "cmake": first_line(["cmake", "--version"]),
        "wsl2": "microsoft" in platform.release().lower(),
        "load_average": [round(value, 3) for value in load_average],
        "ulimit_nofile": {
            "soft": soft_limit,
            "hard": hard_limit,
        },
        "git_head": git_output("rev-parse", "HEAD"),
        "git_status_porcelain": git_output("status", "--porcelain"),
        "working_directory": os.getcwd(),
        "invocation": " ".join(sys.argv),
    }


def read_proc_snapshot(pid):
    try:
        stat_text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
        status_text = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
        fd_count = len(os.listdir(f"/proc/{pid}/fd"))
    except (OSError, ValueError, IndexError):
        return None

    right_paren = stat_text.rfind(")")
    if right_paren < 0:
        return None
    fields = stat_text[right_paren + 2 :].split()
    if len(fields) < 22:
        return None

    status = {}
    for line in status_text.splitlines():
        if ":" in line:
            name, value = line.split(":", 1)
            status[name.strip()] = value.strip()

    def status_int(name):
        value = status.get(name)
        if not value:
            return 0
        try:
            return int(value.split()[0])
        except (ValueError, IndexError):
            return 0

    clock_ticks = os.sysconf("SC_CLK_TCK")
    try:
        user_ticks = int(fields[11])
        system_ticks = int(fields[12])
        threads = int(fields[17])
        rss_pages = int(fields[21])
    except (ValueError, IndexError):
        return None

    page_size = os.sysconf("SC_PAGE_SIZE")
    return {
        "monotonic": time.monotonic(),
        "user_cpu_seconds": user_ticks / clock_ticks,
        "system_cpu_seconds": system_ticks / clock_ticks,
        "cpu_seconds": (user_ticks + system_ticks) / clock_ticks,
        "rss_kib": max(0, rss_pages * page_size // 1024),
        "vm_hwm_kib": status_int("VmHWM"),
        "threads": max(1, threads),
        "fd_count": fd_count,
        "voluntary_context_switches": status_int("voluntary_ctxt_switches"),
        "nonvoluntary_context_switches": status_int("nonvoluntary_ctxt_switches"),
    }


def summarize_samples(points):
    if not points:
        return {"available": False}
    first = points[0]
    last = points[-1]
    elapsed = last["monotonic"] - first["monotonic"]
    cpu_delta = max(0.0, last["cpu_seconds"] - first["cpu_seconds"])
    user_delta = max(0.0, last["user_cpu_seconds"] - first["user_cpu_seconds"])
    system_delta = max(0.0, last["system_cpu_seconds"] - first["system_cpu_seconds"])
    rss_values = [point["rss_kib"] for point in points]
    fd_values = [point["fd_count"] for point in points]
    thread_values = [point["threads"] for point in points]
    return {
        "available": True,
        "sample_count": len(points),
        "elapsed_seconds": round(elapsed, 6),
        "cpu_seconds": round(cpu_delta, 6),
        "user_cpu_seconds": round(user_delta, 6),
        "system_cpu_seconds": round(system_delta, 6),
        "cpu_utilization_percent_one_core": (
            round(cpu_delta / elapsed * 100.0, 3) if elapsed > 0 else None
        ),
        "rss_kib": {
            "min": min(rss_values),
            "mean": round(statistics.fmean(rss_values), 3),
            "max": max(rss_values),
        },
        "max_vm_hwm_kib": max(point["vm_hwm_kib"] for point in points),
        "max_fds": max(fd_values),
        "max_threads": max(thread_values),
        "voluntary_context_switches_delta": max(
            0,
            last["voluntary_context_switches"]
            - first["voluntary_context_switches"],
        ),
        "nonvoluntary_context_switches_delta": max(
            0,
            last["nonvoluntary_context_switches"]
            - first["nonvoluntary_context_switches"],
        ),
    }


class ProcessSampler:
    def __init__(self, pid, label, interval_seconds):
        self.pid = pid
        self.label = label
        self.interval_seconds = interval_seconds
        self._samples = []
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(
            target=self._run,
            name=f"minidb-bench-sampler-{self.label}",
            daemon=True,
        )
        self._thread.start()

    def _run(self):
        while not self._stop.wait(self.interval_seconds):
            snapshot = read_proc_snapshot(self.pid)
            if snapshot is not None:
                self._samples.append(snapshot)

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
        final = read_proc_snapshot(self.pid)
        if final is not None:
            self._samples.append(final)
        summary = summarize_samples(self._samples)
        summary["pid"] = self.pid
        summary["sample_interval_ms"] = round(self.interval_seconds * 1000.0, 3)
        return summary


def open_http_client(port, dataset):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    stream = sock.makefile("rb")

    def one(index):
        key, expected = dataset[index % len(dataset)]
        status, body = http_request(sock, stream, "GET", f"/kv/{key}")
        if status != 200 or body != expected:
            raise RuntimeError(
                f"HTTP GET mismatch for {key}: status={status}, "
                f"bytes={len(body)}"
            )

    def close():
        stream.close()
        sock.close()

    return one, close


def open_kv_client(port, dataset):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def one(index):
        key, expected = dataset[index % len(dataset)]
        sock.sendall(kv_encode(KV_GET, [key]))
        command_type, args = kv_read_frame(sock)
        if command_type != KV_VALUE or len(args) != 1 or args[0] != expected:
            raise RuntimeError(
                f"KV GET mismatch for {key}: command={command_type}"
            )

    def close():
        sock.close()

    return one, close


def measure_round(
    one,
    close,
    warmup_seconds,
    duration_seconds,
    server_pid,
    resource_interval_seconds,
):
    latencies_us = []
    errors = []
    timeout_count = 0
    warmup_requests = 0
    measured_attempts = 0
    elapsed = 0.0
    client_sampler = None
    server_sampler = None
    client_resources = {"available": False}
    server_resources = {"available": False}

    try:
        warmup_deadline = time.perf_counter() + warmup_seconds
        index = 0
        while time.perf_counter() < warmup_deadline:
            try:
                one(index)
            except socket.timeout as exc:
                timeout_count += 1
                errors.append(
                    {
                        "phase": "warmup",
                        "request_index": index,
                        "type": type(exc).__name__,
                        "message": str(exc),
                    }
                )
                break
            except Exception as exc:  # noqa: BLE001
                errors.append(
                    {
                        "phase": "warmup",
                        "request_index": index,
                        "type": type(exc).__name__,
                        "message": str(exc),
                    }
                )
                break
            index += 1
            warmup_requests += 1

        if not errors:
            client_sampler = ProcessSampler(
                os.getpid(), "client", resource_interval_seconds
            )
            server_sampler = ProcessSampler(
                server_pid, "server", resource_interval_seconds
            )
            client_sampler.start()
            server_sampler.start()
            started = time.perf_counter()
            measure_deadline = started + duration_seconds
            while True:
                if time.perf_counter() >= measure_deadline:
                    break
                measured_attempts += 1
                begin = time.perf_counter_ns()
                try:
                    one(index)
                except socket.timeout as exc:
                    timeout_count += 1
                    errors.append(
                        {
                            "phase": "measurement",
                            "request_index": index,
                            "type": type(exc).__name__,
                            "message": str(exc),
                        }
                    )
                    break
                except Exception as exc:  # noqa: BLE001
                    errors.append(
                        {
                            "phase": "measurement",
                            "request_index": index,
                            "type": type(exc).__name__,
                            "message": str(exc),
                        }
                    )
                    break
                latencies_us.append((time.perf_counter_ns() - begin) / 1000.0)
                index += 1
            elapsed = time.perf_counter() - started
    finally:
        if client_sampler is not None:
            client_resources = client_sampler.stop()
        if server_sampler is not None:
            server_resources = server_sampler.stop()
        close()

    successful = len(latencies_us)
    return {
        "warmup_seconds_target": warmup_seconds,
        "warmup_requests": warmup_requests,
        "measurement_seconds_target": duration_seconds,
        "measurement_elapsed_seconds": round(elapsed, 6),
        "requests_attempted": measured_attempts,
        "successful_requests": successful,
        "throughput_requests_per_second": (
            round(successful / elapsed, 3) if elapsed > 0 else 0.0
        ),
        "latency": safe_latency_summary(latencies_us),
        "raw_latency_us": [round(value, 3) for value in latencies_us],
        "error_count": len(errors),
        "timeout_count": timeout_count,
        "errors": errors,
        "in_flight_requests_max": 1,
        "application_queue_depth": (
            "not exposed; closed-loop has at most one in-flight request"
        ),
        "resources": {
            "client": client_resources,
            "server": server_resources,
        },
    }


def aggregate_rounds(rounds):
    all_latencies = []
    for round_result in rounds:
        all_latencies.extend(round_result["raw_latency_us"])
    successful = sum(item["successful_requests"] for item in rounds)
    attempted = sum(item["requests_attempted"] for item in rounds)
    elapsed = sum(item["measurement_elapsed_seconds"] for item in rounds)
    errors = sum(item["error_count"] for item in rounds)
    timeouts = sum(item["timeout_count"] for item in rounds)
    round_qps = [item["throughput_requests_per_second"] for item in rounds]
    client_cpu = [
        item["resources"]["client"]["cpu_utilization_percent_one_core"]
        for item in rounds
        if item["resources"]["client"].get("available")
        and item["resources"]["client"].get(
            "cpu_utilization_percent_one_core"
        )
        is not None
    ]
    server_cpu = [
        item["resources"]["server"]["cpu_utilization_percent_one_core"]
        for item in rounds
        if item["resources"]["server"].get("available")
        and item["resources"]["server"].get(
            "cpu_utilization_percent_one_core"
        )
        is not None
    ]
    return {
        "round_count": len(rounds),
        "successful_requests": successful,
        "requests_attempted": attempted,
        "measurement_seconds": round(elapsed, 6),
        "throughput_requests_per_second": (
            round(successful / elapsed, 3) if elapsed > 0 else 0.0
        ),
        "latency": safe_latency_summary(all_latencies),
        "error_count": errors,
        "timeout_count": timeouts,
        "error_rate": round(errors / attempted, 9) if attempted else None,
        "round_throughput_requests_per_second": numeric_summary(round_qps),
        "client_cpu_utilization_percent_one_core": numeric_summary(client_cpu),
        "server_cpu_utilization_percent_one_core": numeric_summary(server_cpu),
    }


def run_protocol(
    protocol,
    executable,
    dataset,
    work_dir,
    rounds,
    warmup_seconds,
    duration_seconds,
    cooldown_seconds,
    resource_interval_seconds,
):
    port = free_loopback_port()
    rdb_path = os.path.join(work_dir, f"{protocol}.rdb")
    log_path = os.path.join(work_dir, f"{protocol}-server.log")
    server = ServerProcess(executable, port, rdb_path, log_path)
    server.port = port
    round_results = []
    try:
        server.wait_ready(protocol)
        if protocol == "http":
            load_http_dataset(port, dataset)
        else:
            load_kv_dataset(port, dataset)

        for round_index in range(1, rounds + 1):
            if protocol == "http":
                one, close = open_http_client(port, dataset)
            else:
                one, close = open_kv_client(port, dataset)
            result = measure_round(
                one,
                close,
                warmup_seconds,
                duration_seconds,
                server.proc.pid,
                resource_interval_seconds,
            )
            result["round"] = round_index
            round_results.append(result)
            if round_index != rounds:
                time.sleep(cooldown_seconds)

        return_code = server.stop()
        if return_code != 0:
            raise RuntimeError(
                f"{protocol} server exited with {return_code}; log={log_path}"
            )
    except Exception:
        server.stop()
        raise

    aggregate = aggregate_rounds(round_results)
    return {
        "protocol": protocol,
        "operation": "GET",
        "load_model": "closed-loop",
        "connections": 1,
        "outstanding_requests_per_connection": 1,
        "server_executable": executable,
        "server_executable_sha256": sha256_file(executable),
        "server_rdb": rdb_path,
        "server_log": log_path,
        "rounds": round_results,
        "aggregate": aggregate,
        "error_count": aggregate["error_count"],
        "timeout_count": aggregate["timeout_count"],
    }


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run timed multi-round MiniDB acceptance benchmarks"
    )
    parser.add_argument("--http", required=True, help="path to minidb_http_server")
    parser.add_argument("--kv", required=True, help="path to minidb_kv_server")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--warmup-seconds", type=float, default=10.0)
    parser.add_argument("--duration-seconds", type=float, default=30.0)
    parser.add_argument("--cooldown-seconds", type=float, default=5.0)
    parser.add_argument("--resource-interval-ms", type=int, default=250)
    parser.add_argument("--keys", type=int, default=256)
    parser.add_argument("--value-size", type=int, default=256)
    parser.add_argument(
        "--work-dir",
        default=None,
        help="preserve server RDB/log files in this directory",
    )
    parser.add_argument(
        "--out",
        default="bench/results/latest-timed.json",
        help="JSON result path",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if args.rounds <= 0:
        raise SystemExit("rounds must be positive")
    if args.warmup_seconds < 0:
        raise SystemExit("warmup-seconds must be non-negative")
    if args.duration_seconds <= 0:
        raise SystemExit("duration-seconds must be positive")
    if args.cooldown_seconds < 0:
        raise SystemExit("cooldown-seconds must be non-negative")
    if args.resource_interval_ms <= 0:
        raise SystemExit("resource-interval-ms must be positive")
    if args.keys <= 0 or args.value_size <= 0:
        raise SystemExit("keys and value-size must be positive")

    dataset = make_dataset(args.keys, args.value_size)
    resource_interval_seconds = args.resource_interval_ms / 1000.0

    if args.work_dir:
        work_dir = str(Path(args.work_dir).resolve())
        Path(work_dir).mkdir(parents=True, exist_ok=True)
        work_context = contextlib.nullcontext(work_dir)
    else:
        work_context = tempfile.TemporaryDirectory(prefix="minidb-bench-timed-")

    with work_context as active_work_dir:
        http_result = run_protocol(
            "http",
            args.http,
            dataset,
            active_work_dir,
            args.rounds,
            args.warmup_seconds,
            args.duration_seconds,
            args.cooldown_seconds,
            resource_interval_seconds,
        )
        time.sleep(args.cooldown_seconds)
        kv_result = run_protocol(
            "kv",
            args.kv,
            dataset,
            active_work_dir,
            args.rounds,
            args.warmup_seconds,
            args.duration_seconds,
            args.cooldown_seconds,
            resource_interval_seconds,
        )

    output = {
        "schema": "minidb-benchmark-v2",
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "environment": environment_record(),
        "configuration": {
            "mode": "timed-multi-round",
            "load_model": "closed-loop",
            "connections_per_protocol": 1,
            "outstanding_requests_per_connection": 1,
            "dataset_keys": args.keys,
            "value_size_bytes": args.value_size,
            "rounds": args.rounds,
            "warmup_seconds_per_round": args.warmup_seconds,
            "measurement_seconds_per_round": args.duration_seconds,
            "cooldown_seconds_between_rounds": args.cooldown_seconds,
            "resource_sampling_interval_ms": args.resource_interval_ms,
            "request_distribution": (
                "deterministic round-robin over the fixed dataset"
            ),
            "random_seed": "none; dataset and request order are deterministic",
            "clock": "time.perf_counter_ns/perf_counter (monotonic)",
            "percentile_algorithm": (
                "linear interpolation at rank p*(n-1) over all raw samples"
            ),
            "application_queue_depth": (
                "not exposed; closed-loop has at most one in-flight request"
            ),
            "client": "Python standard library socket client",
        },
        "limitations": [
            "WSL2 virtualized networking and shared host scheduling affect latency.",
            "Client and server run on the same host and compete for CPU.",
            "The workload is one persistent connection with one outstanding request; it is a closed-loop baseline, not saturation or open-loop capacity.",
            "Python client CPU and parsing overhead are included in latency samples.",
            "No application queue-depth or network-bandwidth counter is exposed; in-flight depth is bounded by the closed-loop model.",
            "Hardware performance counters may be unavailable under WSL2.",
        ],
        "results": [http_result, kv_result],
    }
    destination = Path(args.out)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "out": str(destination),
                "results": [
                    {
                        "protocol": item["protocol"],
                        "rounds": item["aggregate"]["round_count"],
                        "throughput_requests_per_second": item["aggregate"][
                            "throughput_requests_per_second"
                        ],
                        "p50_us": (item["aggregate"]["latency"] or {}).get(
                            "p50_us"
                        ),
                        "p95_us": (item["aggregate"]["latency"] or {}).get(
                            "p95_us"
                        ),
                        "p99_us": (item["aggregate"]["latency"] or {}).get(
                            "p99_us"
                        ),
                        "errors": item["error_count"],
                        "timeouts": item["timeout_count"],
                        "client_cpu_percent": item["aggregate"][
                            "client_cpu_utilization_percent_one_core"
                        ],
                        "server_cpu_percent": item["aggregate"][
                            "server_cpu_utilization_percent_one_core"
                        ],
                    }
                    for item in output["results"]
                ],
            },
            indent=2,
        )
    )
    has_errors = any(item["error_count"] for item in output["results"])
    has_no_success = any(
        item["aggregate"]["successful_requests"] == 0 for item in output["results"]
    )
    return 1 if has_errors or has_no_success else 0


if __name__ == "__main__":
    sys.exit(main())