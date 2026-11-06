#!/usr/bin/env bash
set -euo pipefail

# Usage: tools/trace_http_smoke.sh <http-server-binary> <output-directory>
server_bin="${1:?http server binary required}"
out_dir="${2:?output directory required}"
port="${MINIDB_TRACE_PORT:-18080}"
rdb_path="$(mktemp --suffix=.rdb)"
server_log="$out_dir/server.log"
server_c_log="$out_dir/server-summary.log"
trace_log="$out_dir/strace.log"
trace_summary="$out_dir/strace-summary.txt"
mkdir -p "$out_dir"
rm -f "$rdb_path"

trace_pid=""
server_pid=""

stop_server() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
    fi
    if [[ -n "$trace_pid" ]]; then
        for _ in $(seq 1 200); do
            if [[ ! -e "/proc/$trace_pid" ]]; then
                break
            fi
            local state
            state="$(ps -o stat= -p "$trace_pid" 2>/dev/null || true)"
            if [[ "$state" == Z* ]]; then
                break
            fi
            sleep 0.05
        done
        if [[ -e "/proc/$trace_pid" ]]; then
            state="$(ps -o stat= -p "$trace_pid" 2>/dev/null || true)"
            if [[ "$state" != Z* ]]; then
                kill -KILL "$server_pid" "$trace_pid" 2>/dev/null || true
            fi
        fi
        wait "$trace_pid" 2>/dev/null || true
    fi
    trace_pid=""
    server_pid=""
}

cleanup() {
    stop_server
    rm -f "$rdb_path"
}
trap cleanup EXIT INT TERM

wait_until_ready() {
    for _ in $(seq 1 200); do
        server_pid="$(pgrep -P "$trace_pid" 2>/dev/null | head -n 1 || true)"
        if curl --noproxy '*' -fsS --max-time 1 \
            "http://127.0.0.1:${port}/health" >/dev/null 2>&1; then
            if [[ -z "$server_pid" ]]; then
                echo "server tracee PID was not found" >&2
                return 1
            fi
            return 0
        fi
        if [[ ! -e "/proc/$trace_pid" ]]; then
            echo "strace exited before the server became ready" >&2
            return 1
        fi
        sleep 0.05
    done
    echo "server did not become ready on port ${port}" >&2
    return 1
}

# The first trace keeps timestamps and per-syscall return latencies.
strace -f -tt -T -s 128 \
    -e trace=accept4,epoll_wait,epoll_ctl,read,write,close,fsync,rename,openat \
    -o "$trace_log" \
    "$server_bin" -p "$port" -r "$rdb_path" >"$server_log" 2>&1 &
trace_pid=$!

wait_until_ready
curl --noproxy '*' -fsS --max-time 5 -X POST --data-binary 'trace-value' \
    "http://127.0.0.1:${port}/kv/trace-key" >/dev/null
curl --noproxy '*' -fsS --max-time 5 \
    "http://127.0.0.1:${port}/kv/trace-key" >/dev/null
stop_server

# The same command set with -c gives a stable syscall-count summary.
strace -f -c -o "$trace_summary" \
    -e trace=accept4,epoll_wait,epoll_ctl,read,write,close,fsync,rename,openat \
    "$server_bin" -p "$port" -r "$rdb_path" >"$server_c_log" 2>&1 &
trace_pid=$!

wait_until_ready
curl --noproxy '*' -fsS --max-time 5 -X POST --data-binary 'trace-value' \
    "http://127.0.0.1:${port}/kv/trace-key" >/dev/null
stop_server