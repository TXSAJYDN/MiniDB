# MiniDB

MiniDB is a compact, single-node educational key-value database implemented in
C++20. It exposes the same in-memory store through two independent front ends:

- a strict HTTP/1.1 subset for inspection and simple automation;
- a binary TCP protocol for API-level CRUD, TTL, and explicit snapshotting.

The network stack, HTTP/binary parsers, event loop, TTL store, and RDB format are
implemented in this repository rather than delegated to a database or web
framework. The project is intended for learning, deterministic testing, and
systems-level benchmarking; it is not a production database.

License: DEFERRED BY OWNER

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
  --root /home/win11/minidb-ah-final-20261106-v3-matrix \
  --evidence /home/win11/minidb-ah-final-20261106-v3-evidence \
  --gtest-source "$PWD/build/_deps/googletest-src"
~~~

The build and evidence roots must be absent or empty; the script never deletes
or reuses them. The final matrix ran 214 CTest cases in every configuration.

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

## Acceptance status

The AI-018 repair and the complete stage A-H technical acceptance are recorded
as PASS in `docs/AI_018_EXECUTION_REPORT.md` and
`docs/A_H_ACCEPTANCE_REPORT.md`.

The accepted code baseline is commit `9da4b48f5fe78e4c2896ad1ca07464361b27e362`.
Its push-triggered GitHub Actions run is
[42182440543](https://github.com/TXSAJYDN/MiniDB/actions/runs/42182440543);
all 9 jobs completed successfully: five build/test matrix entries, TSan,
format, static analysis, and fixed-seed fuzz smoke.

The evidence root is `/home/win11/minidb-ah-final-20261106-v3-evidence` on the
acceptance machine. It contains the six-configuration matrix, Stage B socket
runs, Stage G 64 MiB RDB/recovery measurements, Stage H fuzz/static/benchmark
runs, the CI page capture, and the attempted public contribution record.

The owner has deferred the license decision. No `LICENSE` or `COPYING` file has
been created, and this repository must not be represented as having an approved
open-source license.

A real public contribution attempt was made against
[eclipse-4diac/4diac-forte#1135](https://github.com/eclipse-4diac/4diac-forte/issues/1135):
commit `ac80baaf7df26ff1f3ae50d28b19acece4db9ccf` was created and tested, but
neither the upstream push nor a fork push was authorized and anonymous PR
creation returned HTTP 401. No pull request was created.

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

## Repository status

The implementation, tests, workflows, and acceptance reports are tracked and
pushed on `main`. The only intentionally uncommitted local files in the
authoritative worktree are `-configure.log` and
`docs/AI_013_FINAL_SUMMARY.md`, which are retained as historical artifacts and
must not be deleted or added blindly.

The project remains an educational single-node database. Passing A-H does not
make it production-ready, and the documented feature limits and environment
constraints still apply.
