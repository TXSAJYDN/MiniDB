# Troubleshooting

## First checks

Run commands from the authoritative source tree and confirm the current state:

~~~bash
pwd
git status --short
git rev-parse --abbrev-ref HEAD
git rev-parse --short HEAD
cmake --version
c++ --version
clang++ --version
ctest --test-dir build/debug -N | tail -n 3
~~~

The repository currently has implementation, tests, and most documentation as
untracked files. Do not use reset, checkout, clean, or a broad delete to fix a
build problem; those operations can destroy the only copy of the work.

## Build cannot fetch GoogleTest

Symptom: CMake fails while downloading googletest v1.14.0.

Use an existing local source tree:

~~~bash
cmake -S . -B build/debug   -DCMAKE_BUILD_TYPE=Debug   -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build/_deps/googletest-src"
~~~

Verify the archive pin in CMakeLists.txt if network access is available. Do not
replace the pinned dependency with an unpinned download.

## Tests are not discovered

GoogleTest discovery runs after each test executable is built. If ctest -N is
run immediately after configure but before build, tests may appear missing.
Build first:

~~~bash
cmake --build build/debug -j2
ctest --test-dir build/debug -N | tail -n 3
~~~

The current inventory is 152 tests. A lower count usually means an old build
directory or an incomplete build, not that tests were intentionally removed.

## TSan fails with unexpected memory mapping

On WSL2, ThreadSanitizer may fail at startup because of high-entropy ASLR. Run
the build and test commands through setarch:

~~~bash
setarch x86_64 -R cmake --build build/tsan -j1
setarch x86_64 -R ctest --test-dir build/tsan --output-on-failure
~~~

The acceptance matrix script already does this for TSan. Do not disable TSan and
call the result a TSan pass.

## ASan or UBSan reports an error

Read the first sanitizer report, not just the final CTest line. The project uses
-fno-sanitize-recover=all, so the process should stop on the first detected
issue. Reproduce under a single test filter:

~~~bash
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build/asan-ubsan   --output-on-failure -R 'TestSuite.TestName'
~~~

Check that the combined build has both -fsanitize=address and
-fsanitize=undefined in compile and link commands. A successful normal build is
not evidence that sanitizer flags were present.

## Port is already in use

Symptom: bind or listen fails at startup.

Find the process and choose a different port for the smoke run:

~~~bash
ss -ltnp | grep -E ':(8080|6379)'
/opt/minidb/bin/minidb_http_server -p 18080 -r /tmp/minidb-http.rdb
~~~

Tests should use ephemeral ports. If a test uses a fixed port, that is a test
fixture defect and should be fixed rather than serialized by sleeps.

## The server uses an unexpected RDB path

The default is ./minidb.rdb, relative to the process working directory. Use an
absolute path in production:

~~~bash
/opt/minidb/bin/minidb_kv_server -p 6379 -r /var/lib/minidb/kv/minidb.rdb
~~~

The HTTP and KV processes should never share one RDB path.

## Startup says the RDB is corrupted

A nonzero startup exit with a message such as:

~~~text
Fatal: Failed to load RDB file (file corrupted or inaccessible).
Aborting startup.
~~~

means the load path did not return a valid snapshot. The process intentionally
does not start empty or overwrite the file. Check:

~~~bash
ls -l /var/lib/minidb/kv/minidb.rdb
file /var/lib/minidb/kv/minidb.rdb
stat /var/lib/minidb/kv/minidb.rdb
sha256sum /var/lib/minidb/kv/minidb.rdb
~~~

A missing file is the only expected empty-start case. Any other load error is a
failure. Preserve the file and restore a known-good backup. Do not use truncate,
rm, or a binary rewrite as a diagnostic fix.

Common format causes include bad magic, version other than 2, checksum mismatch,
truncated records, duplicate keys, invalid varints, and trailing bytes.

## Save fails during shutdown

The process exits nonzero when the final snapshot fails. Check the directory
permissions and filesystem:

~~~bash
ls -ld /var/lib/minidb/kv
ls -la /var/lib/minidb/kv
touch /var/lib/minidb/kv/.write-test
rm /var/lib/minidb/kv/.write-test
df -h /var/lib/minidb/kv
mount | grep minidb
~~~

The .tmp.<pid>.<sequence> file is created with O_EXCL. Leftover temporary files
mean a previous save did not complete; do not treat them as snapshots.

If the rename succeeded but directory fsync failed, the log reports durability
unconfirmed. The new file may be visible, but the result is still nonzero. Treat
the snapshot as suspect until the filesystem is repaired and a fresh SAVE
succeeds.

## HTTP returns 400, 413, 414, 431, or 501

These are intentional protocol limits, not necessarily server bugs:

- 400: malformed request line, invalid Host, bad framing, or unsupported
  Upgrade;
- 413: body or pending input exceeds the limit;
- 414: request line or target exceeds the limit;
- 431: header bytes or field count exceeds the limit;
- 501: unsupported Transfer-Encoding or a valid extension method.

Check the request against docs/protocol-http.md. The parser is strict about
HTTP/1.1 and CRLF; an HTTP/1.0 client or a proxy that changes framing will be
rejected deliberately.

## HTTP connection hangs

The server parses one request at a time and resumes buffered requests only after
write completion. A client that sends a partial body or withholds CRLF will
eventually receive 408 and the connection will close. A client that reads
slowly may backpressure the server; the output budget is 1.25 MiB per
connection.

If the process accepts connections but no test progresses, capture a bounded
strace and inspect the server log. Do not add unbounded sleeps to make tests
pass.

## KV returns ERROR and closes

Malformed framing is not resynchronized. Confirm the frame is exactly:

~~~text
frame_len : u32 BE
command   : u8
argc      : u8
arg_len   : u32 BE, repeated
arg       : raw bytes
~~~

frame_len excludes the four-byte prefix, includes command and argument framing,
and must consume the whole frame. Unknown command codes and unrecoverable
framing errors close the connection by design. Command-level argument errors
return ERROR while keeping the connection usable.

## KV TTL looks wrong

- TTL -1 means the key exists with no expiry.
- TTL -2 means missing or expired.
- A non-negative value is remaining milliseconds, rounded down.
- A non-positive EXPIRE removes the key.
- Deadline tracking uses monotonic time; wall-clock jumps do not change a live
  deadline.
- SET clears an existing TTL.

TTL is not exposed through the HTTP route table; use the KV protocol.

## Fuzz run produces an artifact

A non-empty artifact directory means the run found a crash. Preserve the file:

~~~bash
find artifacts -type f -maxdepth 1 -print
~~~

Minimize with -minimize_crash=1, add the input to the appropriate corpus, and
add a durable regression test before rerunning the fixed-seed smoke. Remove the
artifact only after the diagnostic and regression work is complete.

The acceptance run is clean only when both fixed-seed fuzz targets exit 0, show
no ASan/UBSan diagnostic, and leave the artifact directory empty.

## perf reports permission or event errors

WSL2 commonly exposes only a subset of software events. Record the attempted
command and the exact error. Use available counters such as task-clock and
page-faults. Do not publish cycles, instructions, branch misses, cache misses, or
flame graphs that the environment did not provide.

## Extracting a diagnostic bundle

Useful non-destructive evidence:

~~~bash
{
  date -u
  uname -a
  cmake --version
  c++ --version
  clang++ --version
  git rev-parse HEAD
  git status --short
  ss -ltnp
} > /tmp/minidb-diagnostics.txt
~~~

For a failing CTest case:

~~~bash
ctest --test-dir build/debug --output-on-failure   -R 'ExactTestSuite.ExactTestName' -V
~~~

For a server process, use a bounded strace and preserve the server's stdout and
stderr. Do not kill the process with SIGKILL when a graceful SIGTERM is enough.
