# MiniDB

MiniDB is a compact, single-node educational key-value database implemented in
C++20. It exposes the same in-memory store through two independent front ends:

- a strict HTTP/1.1 subset for inspection and simple automation;
- a binary TCP protocol for API-level CRUD, TTL, and explicit snapshotting.

The network stack, HTTP/binary parsers, event loop, TTL store, and RDB format are
implemented in this repository rather than delegated to a database or web
framework. The project is intended for learning, deterministic testing, and
systems-level benchmarking; it is not a production database.

## Implemented P0 capabilities

- Linux **level-triggered epoll** reactor with one owner thread per server.
- Non-blocking accept, incremental reads, short-write handling, backpressure,
  bounded per-connection buffers, and graceful shutdown.
- HTTP/1.1 parser with strict framing and explicit size limits.
- Binary-safe KV protocol with one command in flight per connection.
- In-memory key/value store with lazy and periodic TTL expiry.
- RDB v2 snapshots with CRC32C, bounded allocation, temp-file write,
  file/directory fsync, and atomic rename.
- Recovery into a temporary store followed by atomic publication. Corrupt or
  truncated snapshots are rejected without replacing the live store.
- CLI contract tests for HTTP and KV, persistence restart tests, corrupt-file
  startup tests, and shutdown-save failure tests.

## Deliberate limits

- No TLS, authentication, authorization, replication, clustering, sharding, or
  transactions.
- No HTTP/2, chunked request bodies, `Expect: 100-continue`, or WebSocket
  upgrade handling.
- One event-loop thread handles all client I/O. The TTL reaper is the only
  background thread.
- At most 32 concurrent connections per server process.
- RDB v2 is the only accepted snapshot version; there is no v1 migration path.
- Only one server process may own a given RDB path. There is no inter-process
  file lock.
- Verified primarily on Ubuntu 24.04 under WSL2. Other Linux environments should
  be revalidated, particularly for TSan and performance claims.

## Requirements

- Linux (the authoritative development environment is Ubuntu 24.04 WSL2)
- CMake 3.14 or newer
- GCC 13 or newer, or a recent Clang
- Python 3 for process, benchmark, and fuzz-smoke tooling
- Network access on the first CMake configure so FetchContent can obtain
  GoogleTest v1.14.0, unless a local GoogleTest source tree is supplied

## Build and test

Using CMake presets:

~~~bash
cmake --preset debug
cmake --build --preset debug -j2
ctest --preset debug --output-on-failure
~~~

Or configure explicitly:

~~~bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure
~~~

Presets are provided for Debug, Release, ASan, UBSan, ASan+UBSan, and TSan.
TSan on the current WSL2 kernel must be launched through
`setarch x86_64 -R` to avoid the high-entropy ASLR startup failure.

For a fully fresh six-configuration matrix with preserved logs:

~~~bash
python3 tools/run_acceptance_matrix.py \
  --source "$PWD" \
  --root /home/win11/minidb-final-matrix-v2 \
  --evidence /home/win11/minidb-final-evidence-v2 \
  --gtest-source "$PWD/build/_deps/googletest-src"
~~~

The build and evidence roots must be absent or empty; the script never deletes
or reuses them.

## Run

HTTP server (defaults: port 8080, RDB `minidb.rdb` in the current directory):

~~~bash
build/release/app/minidb_http_server -p 8080 -r /tmp/minidb-http.rdb
~~~

KV server (defaults: port 6379, RDB `minidb.rdb` in the current directory):

~~~bash
build/release/app/minidb_kv_server -p 6379 -r /tmp/minidb-kv.rdb
~~~

Basic HTTP check:

~~~bash
curl -i http://127.0.0.1:8080/health
curl -i -X POST --data-binary 'hello' http://127.0.0.1:8080/kv/greeting
curl -i http://127.0.0.1:8080/kv/greeting
~~~

The binary client smoke script can be run against the KV server:

~~~bash
python3 test_kv_client.py 6379
~~~

SIGINT and SIGTERM trigger a graceful save and process exit. Exit status is 0
only if the final snapshot and parent-directory durability steps succeed.

## Documentation

- [Architecture](docs/architecture.md)
- [HTTP protocol](docs/protocol-http.md)
- [KV binary protocol](docs/protocol-kv.md)
- [RDB v2 format](docs/rdb-format.md)
- [Deployment and operation](docs/deployment.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Benchmark methodology](bench/README.md)
- [Developer tools](tools/README.md)
- [Fuzzing](fuzz/README.md)
- [Dependencies](docs/dependencies.md)
- [Project retrospective](docs/project-retrospective.md)

Acceptance evidence and known remaining external items are recorded in
`docs/AI_018_EXECUTION_REPORT.md` and `docs/A_H_ACCEPTANCE_REPORT.md`. Local
test success is not the same as a public CI run or an accepted upstream
contribution.

## Repository status

The implementation, tests, and most documentation currently exist as
untracked files in the authoritative WSL worktree. The project intentionally
has not committed or pushed them during this repair round. Treat the filesystem
state and the acceptance reports as the source of truth until the owner reviews
and commits the work.
