# Stage B: Linux TCP echo experiments

This directory contains the two deliberately small servers used to study
blocking and non-blocking TCP behavior before relying on the Reactor in
`src/net/`. The experiment is Linux-specific: it uses `accept4`,
non-blocking `fcntl`, `poll`, `MSG_NOSIGNAL`, `SHUT_WR`,
and Linux `errno` values.

## Files

- `socket_echo_blocking.cpp`: one-connection-at-a-time blocking echo
  server. It retries interrupted calls, writes all bytes with
  `MSG_NOSIGNAL`, and reports EOF, reset, and partial-write paths.
- `socket_echo_nonblocking.cpp`: `poll`-driven non-blocking echo
  server. It keeps a bounded pending-output buffer per connection, drains
  accepts and reads to `EAGAIN`, flushes on `POLLOUT`, and
  preserves accepted bytes through a peer half-close.
- `echo_client.py`: deterministic standard-library test client with
  basic, byte-at-a-time, multi-connection, slow-reader, half-close, and
  abrupt-reset cases.
- `../tools/run_stage_b_acceptance.py`: reproducible evidence collector
  for both servers. It verifies exit codes, process fd counts, client byte
  equality, `EAGAIN`, short writes, EOF, RST, optional `strace`,
  and sanitizer output.

## Build

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target   socket_echo_blocking socket_echo_nonblocking -j2
python3 -m py_compile experiments/echo_client.py
```

The experiment inherits the project-wide `-Wall -Wextra -Wpedantic
-Werror` flags. A combined ASan+UBSan build is:

```bash
cmake -S . -B build/asan-ubsan   -DCMAKE_BUILD_TYPE=Debug   -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake --build build/asan-ubsan --target   socket_echo_blocking socket_echo_nonblocking -j2
```

## Run manually

The servers use an ephemeral loopback port when no port is given. Read the
printed port before starting the client.

```bash
# Terminal 1: blocking server
./build/debug/experiments/socket_echo_blocking

# Terminal 2: tests compatible with the one-connection-at-a-time server
python3 experiments/echo_client.py --port PORT --test blocking_all
```

```bash
# Terminal 1: small server send buffer to make backpressure reproducible
./build/debug/experiments/socket_echo_nonblocking --sndbuf 4096

# Terminal 2: complete non-blocking matrix
python3 experiments/echo_client.py --port PORT --test all
```

The blocking server intentionally services one connection until EOF, so the
concurrent `multi_client` case is excluded from `blocking_all`. The
non-blocking server processes all cases and remains alive after peer resets.

## Raw evidence

Run into a **new** evidence directory. The script refuses to reuse a non-empty
directory. It does not delete old evidence.

```bash
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 python3 tools/run_stage_b_acceptance.py   --source .   --build-dir build/asan-ubsan   --evidence /home/win11/minidb-stage-b-evidence-YYYYMMDD-HHMMSS
```

Expected evidence includes:

- `summary.json` with both client/server exit codes and fd counts;
- `py-compile.log` and `build.log`;
- blocking/non-blocking server and client logs;
- `nonblocking-strace.log` when `strace` is installed;
- fd snapshots proving the server returns to its pre-client descriptor count.

Acceptance requires the non-blocking `STATS` line to contain positive
`eagain`, `short_writes`, `read_eof`, and `reset`
counts. Exact counts are timing-dependent and are not treated as fixed
thresholds.

## Observable behavior

The 4 KiB server send buffer and 512 KiB slow-reader test force the
non-blocking server to retain unsent bytes. `send()` then reports a short
write or `EAGAIN`; the pending suffix remains owned by that connection and
is flushed only after `POLLOUT`. No sleep loop blocks other clients.

A peer `shutdown(SHUT_WR)` produces EOF after its final bytes. The server
echoes all accepted bytes, calls `shutdown(SHUT_WR)`, and then closes. A
vector `multi_client` run checks that one slow or resetting peer does not
prevent unrelated clients from making progress.

An abrupt close with `SO_LINGER` set to zero triggers an RST. A subsequent
probe connection must still echo correctly. File-descriptor snapshots are taken
from `/proc/&lt;pid&gt;/fd` before and after the matrix and must match.

## Answers to the stage review questions

1. A TCP `read` observes a byte stream, not request boundaries. The
   client may split one message or combine several writes.
2. When `write`/`send` accepts only a prefix, the application still
   owns the suffix. With a non-blocking descriptor it must retain that suffix
   and wait for writability rather than spinning.
3. EOF is the peer's orderly write-side shutdown. `shutdown(SHUT_WR)`
   sends FIN while leaving the local read side usable. RST is an abnormal reset
   that can discard queued data and surfaces as `ECONNRESET`/`EPIPE`.

The last recorded WSL environment was
`Linux 6.18.54.1-microsoft-standard-WSL2 x86_64` with GCC 13.3.0. Results
from Windows sockets or native Windows are not substituted for these Linux
checks.
