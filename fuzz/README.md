# Fuzz targets

Two Clang libFuzzer targets cover the incremental HTTP/1.1 parser and the binary
KV frame parser. The whole project is instrumented with AddressSanitizer and
UndefinedBehaviorSanitizer; the fuzz targets add libFuzzer's main and driver.

## Build

Building these targets requires Clang:

~~~bash
cmake -S . -B /tmp/minidb-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_FUZZERS=ON \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build/_deps/googletest-src"
cmake --build /tmp/minidb-fuzz -j2 \
  --target minidb_http_parser_fuzz minidb_kv_protocol_fuzz
~~~

## Fixed-seed smoke run

Run from the repository root with a fresh empty artifact directory:

~~~bash
mkdir -p /tmp/minidb-fuzz-artifacts
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
/tmp/minidb-fuzz/fuzz/minidb_http_parser_fuzz \
  -seed=20261105 -runs=20000 -max_len=2097152 \
  -artifact_prefix=/tmp/minidb-fuzz-artifacts/ fuzz/corpus/http

ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
/tmp/minidb-fuzz/fuzz/minidb_kv_protocol_fuzz \
  -seed=20261105 -runs=20000 -max_len=1179648 \
  -artifact_prefix=/tmp/minidb-fuzz-artifacts/ fuzz/corpus/kv

test -z "$(find /tmp/minidb-fuzz-artifacts -type f -print -quit)"
~~~

Both runs must exit 0, report no ASan/UBSan diagnostics, and leave the artifact
directory empty. A crash, timeout, OOM, or artifact file is a failure.

Any crashing input must be minimized with `-minimize_crash=1`, added to the
corpus under a descriptive name, and covered by a durable unit or integration
regression test before the fuzz run can be considered clean.
