#!/usr/bin/env python3
"""Reproducible durability, concurrency, recovery, and checkpoint benchmark.

The harness intentionally uses only the Python standard library.  Every
measurement starts a fresh KV server, preloads a deterministic dataset, warms
all client connections, measures one closed-loop round, kills the server, and
measures recovery from the resulting RDB + WAL image.  Raw latency samples are
kept only in memory; the JSON artifact contains compact summaries.

Results are a WSL2 loopback, single-node, closed-loop workstation baseline.
They are not a production SLA or a capacity claim.
"""

import argparse
import datetime
import json
import os
import platform
import random
import resource
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from benchmark import (  # noqa: E402
    ServerProcess,
    free_loopback_port,
    kv_encode,
    kv_read_frame,
    latency_summary,
)

KV_PING = 1
KV_SET = 2
KV_GET = 3
KV_EXPIRE = 5
KV_TTL = 6
KV_SAVE = 7
KV_STATS = 8
KV_OK = 101
KV_VALUE = 102
KV_NOT_FOUND = 103
KV_INTEGER = 104


def read_stats(port):
    with socket.create_connection(("127.0.0.1", port), timeout=10.0) as sock:
        sock.settimeout(10.0)
        sock.sendall(kv_encode(KV_STATS, []))
        command_type, args = kv_read_frame(sock)
    if command_type != KV_VALUE or len(args) != 1:
        raise RuntimeError(
            f"STATS returned type={command_type}, args={args!r}")
    return json.loads(args[0].decode("utf-8"))


def connect_kv(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
    sock.settimeout(10.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def send_ping(sock):
    sock.sendall(kv_encode(KV_PING, []))
    command_type, _ = kv_read_frame(sock)
    if command_type != KV_OK:
        raise RuntimeError(f"PING returned {command_type}")


def make_dataset(key_count, value_size):
    return [
        (f"key-{index:04d}", bytes([65 + (index % 26)]) * value_size)
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
                    f"dataset preload failed for {key}: {command_type}")
    finally:
        sock.close()


def execute_operation(sock, workload, key, value, ttl_ms):
    if workload == "write" or (workload == "mixed" and value is not None):
        sock.sendall(kv_encode(KV_SET, [key, value]))
        command_type, _ = kv_read_frame(sock)
        return command_type == KV_OK
    if workload == "mixed":
        sock.sendall(kv_encode(KV_GET, [key]))
        command_type, _ = kv_read_frame(sock)
        return command_type in (KV_VALUE, KV_NOT_FOUND)
    if workload == "ttl":
        if value is not None:
            sock.sendall(kv_encode(KV_EXPIRE, [key, str(ttl_ms)]))
            command_type, _ = kv_read_frame(sock)
            return command_type == KV_OK
        sock.sendall(kv_encode(KV_TTL, [key]))
        command_type, _ = kv_read_frame(sock)
        return command_type == KV_INTEGER
    raise ValueError(f"unknown workload {workload!r}")


def warm_clients(port, client_count, dataset, workload, seed):
    rng = random.Random(seed ^ 0x5EED)
    for client_index in range(client_count):
        sock = connect_kv(port)
        try:
            for iteration in range(10):
                key, stored = dataset[(client_index + iteration) % len(dataset)]
                is_mutation = workload == "write" or (
                    workload == "mixed" and rng.random() < 0.20) or (
                    workload == "ttl" and rng.random() < 0.50)
                operation_value = stored if is_mutation else None
                ttl_ms = rng.randint(5000, 60000) if is_mutation else 0
                if not execute_operation(
                        sock, workload, key, operation_value, ttl_ms):
                    raise RuntimeError("warm-up operation was rejected")
        finally:
            sock.close()


def worker(client_index, port, dataset, workload, duration_seconds, seed,
           barrier, result):
    rng = random.Random(seed + client_index * 1000003)
    latencies = []
    attempts = 0
    successes = 0
    errors = 0
    mutation_logical_bytes = 0
    error_samples = []
    sock = None
    try:
        sock = connect_kv(port)
        send_ping(sock)
        barrier.wait(timeout=30.0)
        started = time.monotonic()
        deadline = started + duration_seconds
        while time.monotonic() < deadline:
            key, stored = dataset[rng.randrange(len(dataset))]
            is_mutation = workload == "write" or (
                workload == "mixed" and rng.random() < 0.20) or (
                workload == "ttl" and rng.random() < 0.50)
            value = stored if is_mutation else None
            ttl_ms = rng.randint(5000, 60000) if is_mutation else 0
            began = time.perf_counter_ns()
            attempts += 1
            try:
                if sock is None:
                    sock = connect_kv(port)
                if execute_operation(sock, workload, key, value, ttl_ms):
                    successes += 1
                    latencies.append(
                        (time.perf_counter_ns() - began) / 1000.0)
                    if is_mutation:
                        if workload == "ttl":
                            mutation_logical_bytes += len(key.encode()) + len(
                                str(ttl_ms).encode())
                        else:
                            mutation_logical_bytes += len(key.encode()) + len(
                                stored)
                else:
                    errors += 1
                    if len(error_samples) < 5:
                        error_samples.append("unexpected response")
            except (OSError, RuntimeError, ValueError) as error:
                errors += 1
                if len(error_samples) < 5:
                    error_samples.append(type(error).__name__ + ": " + str(error))
                if sock is not None:
                    try:
                        sock.close()
                    except OSError:
                        pass
                    sock = None
                time.sleep(0.01)
        elapsed = time.monotonic() - started
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
    result.update({
        "latencies_us": latencies,
        "attempts": attempts,
        "successes": successes,
        "errors": errors,
        "mutation_logical_bytes": mutation_logical_bytes,
        "elapsed_seconds": elapsed,
        "error_samples": error_samples,
    })


def read_proc_cpu_ticks(pid):
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        tail = text[text.rfind(")") + 2:].split()
        return int(tail[11]) + int(tail[12])
    except (FileNotFoundError, IndexError, ValueError):
        return 0


def read_rss_kib(pid):
    try:
        for line in Path(f"/proc/{pid}/status").read_text(
                encoding="ascii").splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except (FileNotFoundError, ValueError):
        pass
    return 0


class ResourceSampler:
    def __init__(self, server_pid):
        self.server_pid = server_pid
        self.stop_event = threading.Event()
        self.server_rss = []
        self.client_rss = []
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=5.0)
        return {
            "server_rss_mean_kib": _mean_int(self.server_rss),
            "server_rss_peak_kib": max(self.server_rss, default=0),
            "client_rss_mean_kib": _mean_int(self.client_rss),
            "client_rss_peak_kib": max(self.client_rss, default=0),
        }

    def _run(self):
        while not self.stop_event.wait(0.05):
            server_rss = read_rss_kib(self.server_pid)
            client_rss = read_rss_kib(os.getpid())
            if server_rss:
                self.server_rss.append(server_rss)
            if client_rss:
                self.client_rss.append(client_rss)


def _mean_int(values):
    return round(statistics.fmean(values), 3) if values else 0


def _median(values):
    return round(statistics.median(values), 6) if values else 0.0


def kill_hard(server):
    if server.proc.poll() is None:
        os.killpg(server.proc.pid, signal.SIGKILL)
        server.proc.wait(timeout=10.0)
    server.log_file.close()


def measure_checkpoint(port):
    began = time.perf_counter_ns()
    with connect_kv(port) as sock:
        sock.sendall(kv_encode(KV_SAVE, []))
        command_type, _ = kv_read_frame(sock)
    elapsed_us = (time.perf_counter_ns() - began) / 1000.0
    if command_type != KV_OK:
        raise RuntimeError(f"SAVE returned {command_type}")
    return elapsed_us


def run_round(kv_binary, durability_mode, client_count, workload, dataset,
              duration_seconds, seed, round_index, temp_dir):
    tag = f"{durability_mode}-c{client_count}-{workload}-r{round_index}"
    port = free_loopback_port()
    rdb_path = os.path.join(temp_dir, tag + ".rdb")
    log_path = os.path.join(temp_dir, tag + "-server.log")
    server = ServerProcess(kv_binary, port, rdb_path, log_path,
                           durability_mode=durability_mode)
    recovery_server = None
    try:
        server.wait_ready("kv")
        preload_dataset(port, dataset)
        warm_clients(port, client_count, dataset, workload,
                     seed + round_index * 7919)
        if durability_mode == "interval":
            time.sleep(0.15)

        stats_before = read_stats(port)
        barrier = threading.Barrier(client_count)
        results = [dict() for _ in range(client_count)]
        threads = [
            threading.Thread(
                target=worker,
                args=(index, port, dataset, workload, duration_seconds,
                      seed + round_index * 104729, barrier, results[index]),
                daemon=True,
            )
            for index in range(client_count)
        ]

        sampler = ResourceSampler(server.proc.pid)
        server_ticks_before = read_proc_cpu_ticks(server.proc.pid)
        client_cpu_before = resource.getrusage(resource.RUSAGE_SELF)
        sampler.start()
        wall_started = time.monotonic()
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=duration_seconds + 30.0)
            if thread.is_alive():
                raise RuntimeError("benchmark worker did not stop")
        elapsed = time.monotonic() - wall_started
        sampler_result = sampler.stop()
        server_ticks_after = read_proc_cpu_ticks(server.proc.pid)
        client_cpu_after = resource.getrusage(resource.RUSAGE_SELF)

        latencies = []
        attempts = 0
        successes = 0
        errors = 0
        mutation_bytes = 0
        error_samples = []
        for result in results:
            latencies.extend(result.get("latencies_us", []))
            attempts += result.get("attempts", 0)
            successes += result.get("successes", 0)
            errors += result.get("errors", 0)
            mutation_bytes += result.get("mutation_logical_bytes", 0)
            error_samples.extend(result.get("error_samples", []))
        if successes == 0:
            raise RuntimeError(f"{tag} completed no successful requests")

        stats_after = read_stats(port)
        wal_bytes = max(
            0, stats_after["wal"]["bytes_written"] -
            stats_before["wal"]["bytes_written"])
        fsyncs = max(
            0, stats_after["wal"]["data_fsyncs"] -
            stats_before["wal"]["data_fsyncs"])
        interval_flushes = max(
            0, stats_after["persistence"]["interval_flushes"] -
            stats_before["persistence"]["interval_flushes"])
        write_amplification = (
            wal_bytes / mutation_bytes if mutation_bytes else 0.0)
        clock_ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
        server_cpu_seconds = max(
            0, server_ticks_after - server_ticks_before) / clock_ticks
        client_cpu_seconds = (
            (client_cpu_after.ru_utime + client_cpu_after.ru_stime) -
            (client_cpu_before.ru_utime + client_cpu_before.ru_stime))
        sample = {
            "round": round_index,
            "attempts": attempts,
            "successes": successes,
            "errors": errors,
            "error_rate": round(errors / attempts, 8) if attempts else 0.0,
            "error_samples": error_samples[:5],
            "elapsed_seconds": round(elapsed, 6),
            "throughput_ops_per_second": round(successes / elapsed, 3),
            "latency": latency_summary(latencies),
            "client_cpu_percent_one_core": round(
                client_cpu_seconds / elapsed * 100.0, 3),
            "server_cpu_percent_one_core": round(
                server_cpu_seconds / elapsed * 100.0, 3),
            **sampler_result,
            "wal_bytes": wal_bytes,
            "wal_data_fsyncs": fsyncs,
            "interval_flushes": interval_flushes,
            "mutation_logical_bytes": mutation_bytes,
            "write_amplification": round(write_amplification, 6),
            "wal_bytes_at_kill": stats_after["wal_bytes"],
        }

        if durability_mode == "interval":
            time.sleep(0.15)
        kill_hard(server)

        recovery_port = free_loopback_port()
        recovery_log = os.path.join(temp_dir, tag + "-recovery.log")
        recovery_started = time.monotonic()
        recovery_server = ServerProcess(
            kv_binary, recovery_port, rdb_path, recovery_log,
            durability_mode=durability_mode)
        recovery_server.wait_ready("kv")
        recovery_seconds = time.monotonic() - recovery_started
        recovery_stats = read_stats(recovery_port)
        sample["recovery_seconds"] = round(recovery_seconds, 6)
        sample["recovery"] = recovery_stats["recovery"]
        sample["checkpoint_save_us"] = round(
            measure_checkpoint(recovery_port), 3)
        stats_checkpoint = read_stats(recovery_port)
        sample["checkpoint_pause_us"] = max(
            0, stats_checkpoint["persistence"]["checkpoint_pause_us"] -
            recovery_stats["persistence"]["checkpoint_pause_us"])
        shutdown_result = recovery_server.stop()
        recovery_server = None
        if shutdown_result != 0:
            raise RuntimeError(
                f"{tag} recovery server shutdown returned {shutdown_result}")

        sample["latencies_us"] = latencies
        return sample
    finally:
        if recovery_server is not None:
            try:
                recovery_server.stop()
            except Exception:
                pass
        if server.proc.poll() is None:
            try:
                server.stop()
            except Exception:
                pass
        elif not server.log_file.closed:
            server.log_file.close()


def aggregate_configuration(mode, client_count, workload, rounds):
    pooled_latencies = []
    for item in rounds:
        pooled_latencies.extend(item["latencies_us"])
    attempts = sum(item["attempts"] for item in rounds)
    successes = sum(item["successes"] for item in rounds)
    errors = sum(item["errors"] for item in rounds)
    latencies = latency_summary(pooled_latencies)
    compact_rounds = []
    for item in rounds:
        compact_rounds.append({
            "round": item["round"],
            "throughput_ops_per_second": item["throughput_ops_per_second"],
            "latency": item["latency"],
            "error_rate": item["error_rate"],
            "client_cpu_percent_one_core":
                item["client_cpu_percent_one_core"],
            "server_cpu_percent_one_core":
                item["server_cpu_percent_one_core"],
            "server_rss_peak_kib": item["server_rss_peak_kib"],
            "wal_bytes": item["wal_bytes"],
            "wal_data_fsyncs": item["wal_data_fsyncs"],
            "write_amplification": item["write_amplification"],
            "checkpoint_save_us": item["checkpoint_save_us"],
            "checkpoint_pause_us": item["checkpoint_pause_us"],
            "recovery_seconds": item["recovery_seconds"],
            "recovery": item["recovery"],
        })
    return {
        "durability_mode": mode,
        "clients": client_count,
        "workload": workload,
        "round_count": len(rounds),
        "attempts": attempts,
        "successes": successes,
        "errors": errors,
        "error_rate": round(errors / attempts, 8) if attempts else 0.0,
        "throughput_ops_per_second_median": _median(
            [item["throughput_ops_per_second"] for item in rounds]),
        "throughput_ops_per_second_min": round(min(
            item["throughput_ops_per_second"] for item in rounds), 3),
        "throughput_ops_per_second_max": round(max(
            item["throughput_ops_per_second"] for item in rounds), 3),
        "pooled_latency": latencies,
        "client_cpu_percent_one_core_median": _median(
            [item["client_cpu_percent_one_core"] for item in rounds]),
        "server_cpu_percent_one_core_median": _median(
            [item["server_cpu_percent_one_core"] for item in rounds]),
        "client_rss_peak_kib_max": max(
            item["client_rss_peak_kib"] for item in rounds),
        "server_rss_peak_kib_max": max(
            item["server_rss_peak_kib"] for item in rounds),
        "wal_bytes_median": _median([item["wal_bytes"] for item in rounds]),
        "wal_data_fsyncs_median": _median(
            [item["wal_data_fsyncs"] for item in rounds]),
        "write_amplification_median": _median(
            [item["write_amplification"] for item in rounds]),
        "checkpoint_save_us_median": _median(
            [item["checkpoint_save_us"] for item in rounds]),
        "checkpoint_pause_us_median": _median(
            [item["checkpoint_pause_us"] for item in rounds]),
        "recovery_seconds_median": _median(
            [item["recovery_seconds"] for item in rounds]),
        "rounds": compact_rounds,
    }


def command_output(command, cwd=None):
    try:
        result = subprocess.run(
            command, cwd=cwd, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=5.0)
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


def environment_record():
    repo_root = Path(__file__).resolve().parent.parent
    status = subprocess.run(
        ["git", "status", "--porcelain"], cwd=repo_root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False).stdout
    return {
        "platform": platform.platform(),
        "kernel": os.uname().release,
        "wsl2": "microsoft" in platform.release().lower(),
        "machine": platform.machine(),
        "cpu_model": cpu_model(),
        "cpu_count": os.cpu_count(),
        "python": platform.python_version(),
        "compiler": command_output(["c++", "--version"]),
        "cmake": command_output(["cmake", "--version"]),
        "filesystem": command_output(
            ["stat", "-f", "-c", "%T", str(repo_root)]),
        "git_head": command_output(["git", "rev-parse", "HEAD"], repo_root),
        "working_tree_dirty_at_collection": bool(status.strip()),
        "load_average_at_start": [round(value, 3)
                                   for value in os.getloadavg()],
    }


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kv", required=True,
                        help="path to minidb_kv_server")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--duration-seconds", type=float, default=2.0)
    parser.add_argument("--dataset-keys", type=int, default=128)
    parser.add_argument("--value-size", type=int, default=64)
    parser.add_argument("--seed", type=int, default=20261111)
    parser.add_argument("--modes", nargs="+", default=["sync", "interval"])
    parser.add_argument("--clients", nargs="+", type=int,
                        default=[1, 8, 32])
    parser.add_argument("--workloads", nargs="+",
                        default=["write", "mixed", "ttl"])
    parser.add_argument("--out",
                        default="bench/results/reliability-20261111.json")
    return parser.parse_args()


def validate_args(args):
    if args.rounds < 5:
        raise SystemExit("--rounds must be at least 5 for acceptance evidence")
    if args.duration_seconds <= 0:
        raise SystemExit("--duration-seconds must be positive")
    if args.dataset_keys <= 0 or args.value_size <= 0:
        raise SystemExit("dataset keys and value size must be positive")
    for mode in args.modes:
        if mode not in ("sync", "interval"):
            raise SystemExit(f"unsupported mode: {mode}")
    for clients in args.clients:
        if clients < 1 or clients > 32:
            raise SystemExit("client count must be in 1..32")
    for workload in args.workloads:
        if workload not in ("write", "mixed", "ttl"):
            raise SystemExit(f"unsupported workload: {workload}")


def main():
    args = parse_args()
    validate_args(args)
    dataset = make_dataset(args.dataset_keys, args.value_size)
    configurations = []
    total_started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="minidb-reliability-bench-") as temp_dir:
        for mode in args.modes:
            for client_count in args.clients:
                for workload in args.workloads:
                    print(
                        f"running mode={mode} clients={client_count} "
                        f"workload={workload}", file=sys.stderr, flush=True)
                    rounds = [
                        run_round(
                            args.kv, mode, client_count, workload, dataset,
                            args.duration_seconds, args.seed, round_index,
                            temp_dir)
                        for round_index in range(1, args.rounds + 1)
                    ]
                    configurations.append(aggregate_configuration(
                        mode, client_count, workload, rounds))

    output = {
        "schema": "minidb-reliability-benchmark-v1",
        "generated_at": datetime.datetime.now(
            datetime.timezone.utc).isoformat(),
        "environment": environment_record(),
        "configuration": {
            "rounds": args.rounds,
            "duration_seconds_per_round": args.duration_seconds,
            "dataset_keys": args.dataset_keys,
            "value_size_bytes": args.value_size,
            "seed": args.seed,
            "modes": args.modes,
            "clients": args.clients,
            "workloads": args.workloads,
            "connection_model":
                "closed-loop persistent KV connections, one in-flight command per client",
            "workload_definitions": {
                "write": "SET with a 64-byte value",
                "mixed": "80% GET, 20% SET",
                "ttl": "50% EXPIRE with 5-60 second TTL, 50% TTL",
            },
            "recovery_method":
                "SIGKILL after the measured round, then fresh-process startup to PING",
            "checkpoint_method":
                "KV SAVE after recovery, measured client-side and via STATS checkpoint_pause_us",
        },
        "limits": {
            "concurrent_connections_per_server": 32,
            "wal_segment_bytes": 16 * 1024 * 1024,
            "wal_total_bytes": 256 * 1024 * 1024,
            "wal_segments": 128,
            "recovery_records": 1000000,
            "recovery_bytes": 512 * 1024 * 1024,
        },
        "limitations": [
            "WSL2 loopback, single node, same-host client and server; no bare-metal or production SLA claim.",
            "Closed-loop clients with one outstanding command each; this is not open-loop saturation capacity.",
            "The Python standard-library client competes for CPU and contributes to measured latency.",
            "The 32-client configuration reaches the server's hard connection cap; it is not evidence for more than 32 clients.",
            "SYNC includes one WAL data fsync per successful mutation on the owner event loop.",
            "INTERVAL acknowledges after append and relies on a 100 ms timer or checkpoint/shutdown flush.",
            "Recovery is measured at one quiescent crash point after each round, not across every possible crash point.",
            "The compact artifact intentionally omits raw latency samples; per-round percentiles and variation are retained.",
        ],
        "configurations": configurations,
        "elapsed_wall_seconds": round(time.monotonic() - total_started, 3),
    }
    destination = Path(args.out)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "out": str(destination),
        "configurations": [
            {
                "mode": item["durability_mode"],
                "clients": item["clients"],
                "workload": item["workload"],
                "qps_median": item["throughput_ops_per_second_median"],
                "p99_us": item["pooled_latency"]["p99_us"],
                "p999_us": item["pooled_latency"]["p999_us"],
                "errors": item["errors"],
                "recovery_seconds_median":
                    item["recovery_seconds_median"],
            }
            for item in configurations
        ],
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
