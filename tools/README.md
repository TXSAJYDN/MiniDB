# Developer tools

## Six-configuration acceptance matrix

`tools/run_acceptance_matrix.py` configures and builds Debug, Release, ASan,
UBSan, ASan+UBSan, and TSan from scratch, then runs CTest in every
configuration. TSan build and test commands are wrapped with
`setarch x86_64 -R` because the current WSL2 kernel otherwise fails with
ThreadSanitizer's "unexpected memory mapping" startup error.

The script:

- refuses to reuse a non-empty build or evidence root;
- records a SHA-256 digest over CMake, source, app, tests, fuzz, bench,
  experiments, and tools files, excluding build products, backups, caches, and `bench/results`;
- records the toolchain, Git HEAD/status, exact commands, and every exit code;
- requires 214 discovered CTest cases per configuration;
- verifies that the combined sanitizer compile and link flags contain both
  AddressSanitizer and UndefinedBehaviorSanitizer, checking each flag
  independently;

Example:

~~~bash
python3 tools/run_acceptance_matrix.py \
  --source /mnt/e/c++/miniDB \
  --root /home/win11/minidb-final-matrix \
  --evidence /home/win11/minidb-final-evidence \
  --gtest-source /home/win11/minidb-gtest-src
~~~

Expected evidence includes `provenance.txt`, per-configuration
`configure.log`/`build.log`/`ctest.log`, and `matrix-summary.json`.

## Stage G RDB footprint and barrier evidence

`tools/run_stage_g_acceptance.py` builds an RDB v2 image at the exact 64 MiB
format limit, measures startup-recovery peak RSS from `/proc/<pid>/status`, and
performs an online HTTP SAVE while recording peak RSS and end-to-end request
latency. It freezes the process RSS budget at 512 MiB and the observable SAVE
pause limit at 30 seconds, then fails unless both limits, the exact-on-limit
file size, the 64-key recovery, and temporary-file cleanup all pass.

Use a non-sanitized Release build and a new evidence directory:

~~~bash
python3 tools/run_stage_g_acceptance.py \
  --source /mnt/e/c++/miniDB \
  --build-dir /home/win11/minidb-final-matrix/release \
  --evidence /home/win11/minidb-final-evidence/stage-g
~~~

The JSON summary, VmRSS/VmHWM sample streams, server logs, full RDB SHA-256,
and the exact command are retained under the evidence directory.

## Static checks

~~~bash
find src app tests fuzz experiments -type f \( -name '*.h' -o -name '*.cpp' \) \
  -print0 | xargs -0 clang-format --dry-run --Werror

cmake -S . -B /tmp/minidb-tidy \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
find src app -type f -name '*.cpp' -print0 | \
  xargs -0 -n1 clang-tidy -p /tmp/minidb-tidy
~~~

CI uses the system Clang tools; local evidence records the exact versions.

## Fuzz targets

See [fuzz/README.md](../fuzz/README.md). The clean-smoke requirement includes a
fixed seed and an empty artifact directory after both runs.

## HTTP syscall trace

`tools/trace_http_smoke.sh` records one HTTP process from startup through
POST/GET and SIGTERM save. It stores timestamped syscall latency and an
`strace -c` summary.

~~~bash
tools/trace_http_smoke.sh \
  /home/win11/minidb-final-matrix/release/app/minidb_http_server \
  /home/win11/minidb-final-evidence/strace
~~~

Use `MINIDB_TRACE_PORT` to override the default loopback port. The script uses
a temporary RDB and removes it on exit.

## Benchmark

`bench/benchmark.py` is documented in [bench/README.md](../bench/README.md).
Record the raw JSON rather than transcribing only throughput numbers.

## Performance counters

WSL2 may reject hardware performance events. Record the attempted `perf` command
and the exact denial, and use only counters the environment actually exposes
(such as `task-clock` or `page-faults`). Never fabricate cycles, instructions,
branches, cache misses, or flame graphs.

## Process and protocol smoke tests

`tests/process_integration_test.py` covers CLI validation, default RDB paths,
HTTP/KV restart persistence, corrupt-RDB startup failure, and save-failure exit
status. It is registered in CTest as six process integration cases. It uses
ephemeral loopback ports and temporary directories, and it does not assume a
single read returns a complete response.
