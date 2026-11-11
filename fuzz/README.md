# Fuzz targets

Six Clang libFuzzer targets cover the protocol, persistence, and Agent state
surfaces:

| Target | Input surface | Fixed corpus |
|---|---|---|
| `minidb_http_parser_fuzz` | incremental HTTP/1.1 parser | `fuzz/corpus/http` |
| `minidb_kv_protocol_fuzz` | binary KV frame parser and encoder | `fuzz/corpus/kv` |
| `minidb_wal_record_fuzz` | WAL record codec and CRC checks | `fuzz/corpus/wal` |
| `minidb_checkpoint_manifest_fuzz` | checkpoint manifest codec and validation | `fuzz/corpus/manifest` |
| `minidb_agent_record_codec_fuzz` | Agent marker, key, payload, record, and response codecs | `fuzz/corpus/agent_record` |
| `minidb_agent_protocol_fuzz` | stateful Agent subcommand parser and committed-state transitions | `fuzz/corpus/agent_protocol` |

The whole project is instrumented with AddressSanitizer and UndefinedBehaviorSanitizer;
each fuzz target additionally links libFuzzer's main and driver.

The Agent protocol target accepts up to 256 KiB of input. Its first byte is
treated as a bootstrap selector when it is odd: the target applies a fixed,
valid session/lease/tool/record state transition sequence before parsing the
remaining bytes. Even first bytes are parsed directly as action streams. Each
action contains a one-byte command selector, a one-byte argument count, and
little-endian `u16` length-prefixed arguments. At most 16 actions are executed
per input. This exercises both valid state-machine transitions and malformed
length/count/idempotency inputs without persisting data outside the process.

## Build

Building these targets requires Clang:

```bash
cmake -S . -B /tmp/minidb-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_FUZZERS=ON \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="${PWD}/build/_deps/googletest-src"
cmake --build /tmp/minidb-fuzz -j2 \
  --target minidb_http_parser_fuzz \
           minidb_kv_protocol_fuzz \
           minidb_wal_record_fuzz \
           minidb_checkpoint_manifest_fuzz \
           minidb_agent_record_codec_fuzz \
           minidb_agent_protocol_fuzz
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

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
/tmp/minidb-fuzz/fuzz/minidb_agent_record_codec_fuzz \
  -seed=20261111 -runs=20000 -max_len=70000 \
  -artifact_prefix=/tmp/minidb-fuzz-artifacts/ fuzz/corpus/agent_record

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
/tmp/minidb-fuzz/fuzz/minidb_agent_protocol_fuzz \
  -seed=20261111 -runs=20000 -max_len=262144 \
  -artifact_prefix=/tmp/minidb-fuzz-artifacts/ fuzz/corpus/agent_protocol

test -z "$(find /tmp/minidb-fuzz-artifacts -type f -print -quit)"
```

All six runs must exit 0, report no ASan/UBSan diagnostics, and leave the
artifact directory empty. A crash, timeout, OOM, or artifact file is a failure.

Any crashing input must be minimized with `-minimize_crash=1`, added to the
corresponding corpus under a descriptive name, and covered by a durable unit
or integration regression test before the run can be considered clean.

## CI

The GitHub Actions fuzz-smoke job builds and runs exactly these six targets
with the same seed and run count, then fails if the artifact directory is
non-empty. The local evidence is recorded in
[`docs/reliability-evidence.md`](../docs/reliability-evidence.md).
