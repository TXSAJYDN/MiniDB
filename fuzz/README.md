# Fuzz targets

Four Clang libFuzzer targets cover the protocol and persistence parsers:

| Target | Input surface | Fixed corpus |
|---|---|---|
| `minidb_http_parser_fuzz` | incremental HTTP/1.1 parser | `fuzz/corpus/http` |
| `minidb_kv_protocol_fuzz` | binary KV frame parser and encoder | `fuzz/corpus/kv` |
| `minidb_wal_record_fuzz` | WAL record codec and CRC checks | `fuzz/corpus/wal` |
| `minidb_checkpoint_manifest_fuzz` | checkpoint manifest codec and validation | `fuzz/corpus/manifest` |

The whole project is instrumented with AddressSanitizer and UndefinedBehaviorSanitizer;
each fuzz target additionally links libFuzzer's main and driver.

## Build

Building these targets requires Clang:

```bash
cmake -S . -B /tmp/minidb-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_FUZZERS=ON \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build/_deps/googletest-src"
cmake --build /tmp/minidb-fuzz -j2 \
  --target minidb_http_parser_fuzz \
           minidb_kv_protocol_fuzz \
           minidb_wal_record_fuzz \
           minidb_checkpoint_manifest_fuzz
```

## Fixed-seed smoke run

Run from the repository root with a fresh empty artifact directory. The
acceptance seed is `20261111` and every target runs 20,000 inputs:

```bash
rm -rf /tmp/minidb-fuzz-artifacts
mkdir -p /tmp/minidb-fuzz-artifacts

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
/tmp/minidb-fuzz/fuzz/minidb_http_parser_fuzz \
  -seed=20261111 -runs=20000 -max_len=2097152 \
  -artifact_prefix=/tmp/minidb-fuzz-artifacts/ fuzz/corpus/http

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
/tmp/minidb-fuzz/fuzz/minidb_kv_protocol_fuzz \
  -seed=20261111 -runs=20000 -max_len=1179648 \
  -artifact_prefix=/tmp/minidb-fuzz-artifacts/ fuzz/corpus/kv

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
/tmp/minidb-fuzz/fuzz/minidb_wal_record_fuzz \
  -seed=20261111 -runs=20000 -max_len=1050000 \
  -artifact_prefix=/tmp/minidb-fuzz-artifacts/ fuzz/corpus/wal

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
/tmp/minidb-fuzz/fuzz/minidb_checkpoint_manifest_fuzz \
  -seed=20261111 -runs=20000 -max_len=2048 \
  -artifact_prefix=/tmp/minidb-fuzz-artifacts/ fuzz/corpus/manifest

test -z "$(find /tmp/minidb-fuzz-artifacts -type f -print -quit)"
```

All four runs must exit 0, report no ASan/UBSan diagnostics, and leave the
artifact directory empty. A crash, timeout, OOM, or artifact file is a failure.

Any crashing input must be minimized with `-minimize_crash=1`, added to the
corresponding corpus under a descriptive name, and covered by a durable unit
or integration regression test before the run can be considered clean.

## CI

The GitHub Actions fuzz-smoke job builds and runs exactly these four targets
with the same seed and run count, then fails if the artifact directory is
non-empty. The local evidence is recorded in
[`docs/reliability-evidence.md`](../docs/reliability-evidence.md).
