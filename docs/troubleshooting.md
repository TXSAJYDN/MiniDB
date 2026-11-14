# Troubleshooting

This guide targets the current R0-R4-D reliability implementation: WAL + RDB
checkpoint + manifest recovery, `SYNC` and `INTERVAL` durability, the
single-writer lock, bounded group commit, snapshot-isolation transactions, and
the durable Agent state backend. It is diagnostic guidance for a local
engineering prototype, not a production runbook.

## First checks

Run commands from the authoritative WSL source tree and confirm the current
state:

~~~bash
pwd
git status --short
git rev-parse --abbrev-ref HEAD
git rev-parse --short HEAD
cmake --version
c++ --version
clang++ --version
ctest --test-dir build/release -N | tail -n 3
~~~

The current post-R4-D HTTP-hardening plus `MSET` inventory is 459 CTest cases
in the Debug build, including 32 `ProcessIntegrationTest.*` cases. The
historical R4-D six-configuration matrix remains 440/440 per configuration.
Always identify the build directory and configuration you are actually using;
an old or partial build can report a different count.

Do not use reset, checkout, clean, or a broad delete to fix a build problem.
Preserve persistence images and test artifacts until the cause is understood.

## Build cannot fetch GoogleTest

Symptom: CMake fails while downloading the pinned GoogleTest release.

Use an existing local source tree:

~~~bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build/_deps/googletest-src"
~~~

Verify the archive pin in `CMakeLists.txt` if network access is available. Do
not replace the pinned dependency with an unpinned download.

## Tests are not discovered

GoogleTest discovery runs after each test executable is built. Build first:

~~~bash
cmake --build build/debug -j2
ctest --test-dir build/debug -N | tail -n 3
~~~

The current Debug inventory is 459 tests. A lower count usually means an old build
directory or an incomplete build. The 214 count in
`docs/A_H_ACCEPTANCE_REPORT.md` is historical pre-WAL evidence and is not the
current inventory.

## A process integration test is missing

The current process suite has 32 cases and covers CLI validation, HTTP/KV
persistence, TTL scanner lifecycle, corrupt startup, save failure, crash
recovery, atomic `MSET` crash recovery, interval flush, writer-lock conflict,
WAL tail truncation, manifest corruption, missing WAL segment, middle-of-file
WAL corruption, legacy import, random kill replay, TTL crash recovery, async
mutation ordering, pipelined HEAD/GET ordering, group-commit stats, shutdown
failure exit codes, the stats contract, the transaction contract, Agent
recovery/fencing, and the background-checkpoint rename-before-manifest crash.

Run only that suite in Release:

~~~bash
ctest --preset release -R '^ProcessIntegrationTest\.' --output-on-failure
~~~

If fewer than 32 are discovered, rebuild the full test target rather than
running `process_integration_test.py` ad hoc outside CTest.

## TSan fails with unexpected memory mapping

On WSL2, ThreadSanitizer may fail at startup because of high-entropy ASLR. Run
the build and test commands through setarch:

~~~bash
setarch x86_64 -R cmake --build --preset tsan -j8
setarch x86_64 -R ctest --preset tsan --output-on-failure
~~~

Do not disable TSan and call the result a TSan pass.

## ASan or UBSan reports an error

Read the first sanitizer report, not just the final CTest line. The project uses
`-fno-sanitize-recover=all`, so the process should stop on the first detected
issue. Reproduce under one test filter:

~~~bash
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build/asan-ubsan --output-on-failure -R 'TestSuite.TestName'
~~~

Check that the combined build has both `-fsanitize=address` and
`-fsanitize=undefined` in compile and link commands. A successful normal build
is not evidence that sanitizer flags were present.

## Port is already in use

Symptom: bind or listen fails at startup.

Find the process and choose a different port for the smoke run:

~~~bash
ss -ltnp | grep -E ':(8080|6379)'
/opt/minidb/bin/minidb_http_server -p 18080 -r /tmp/minidb-http/minidb.rdb
~~~

Tests should use ephemeral ports. If a test uses a fixed port, fix the fixture
rather than serializing tests with sleeps.

## The server uses an unexpected persistence path

The default is `./minidb.rdb`, relative to the process working directory. Use an
absolute path in deployment:

~~~bash
/opt/minidb/bin/minidb_kv_server -p 6379 -r /var/lib/minidb/kv/minidb.rdb -d sync
~~~

The HTTP and KV processes must use different base paths. Both processes create
sidecars next to the configured `-r` path:

~~~text
<rdb_path>
<rdb_path>.manifest
<rdb_path>.wal/
<rdb_path>.lock
~~~

## Writer lock conflict

Symptom: startup fails while acquiring `<rdb_path>.lock`, or a second server
exits nonzero even though the first server looks idle.

MiniDB takes an exclusive non-blocking `flock` on that file for the process
lifetime. The lock is advisory: it prevents cooperating MiniDB processes from
sharing one base path, but it cannot stop a third-party program from writing the
files directly.

Check for a live owner and do not delete the lock file as a repair:

~~~bash
pgrep -af 'minidb_http_server|minidb_kv_server'
lslocks | grep minidb || true
~~~

Use a different base path for a second instance. A stale lock filename without a
live holder is harmless; the kernel releases `flock` when the owning fd closes.

## Missing RDB, manifest, or WAL

Startup treats only a complete absence of RDB, manifest, and WAL as an empty
database. An existing standalone RDB with no manifest or WAL is imported as a
legacy image and bootstrapped on the first persistence write or checkpoint.

A missing manifest with existing WAL segments is rejected rather than guessed.
A manifest that references a missing RDB or missing WAL segment is also fatal.
Preserve every remaining file and restore a complete known-good backup.

~~~bash
ls -la /var/lib/minidb/kv
find /var/lib/minidb/kv/minidb.rdb.wal -maxdepth 1 -type f -printf '%f %s\n' | sort
~~~

Do not create an empty manifest, delete WAL files, or force an empty start to
make a damaged image boot.

## WAL tail was truncated during recovery

Only the final WAL segment may end in a partial record. Startup validates the
last record boundary, truncates the incomplete suffix, and fsyncs the repaired
segment. This is expected after a crash in the middle of a WAL append and is
recorded in the recovery statistics as `truncated_bytes`.

Check the startup log and stats. Do not manually truncate a middle segment or a
complete-looking record with a bad checksum; those are corruption errors.

## WAL middle corruption or LSN gap

A bad checksum, malformed record, LSN gap, or impossible segment ordering in the
middle of the WAL is fatal. The process exits nonzero, does not listen, and does
not overwrite the image. Recovery does not silently skip the damaged region.

Check file names, sizes, and hashes before changing anything:

~~~bash
find /var/lib/minidb/kv/minidb.rdb.wal -maxdepth 1 -type f -printf '%f %s\n' | sort
sha256sum /var/lib/minidb/kv/minidb.rdb /var/lib/minidb/kv/minidb.rdb.manifest
~~~

The only automatic WAL repair is truncating an incomplete record at the tail of
the final segment.

## Manifest is missing or corrupt

The manifest records `checkpoint_lsn` and the complete retained WAL segment set starting at `checkpoint_lsn + 1`. It is
published with a temporary file, fsync, atomic rename, and parent-directory
fsync. A corrupt manifest, a missing manifest with existing WAL, or a manifest
that points at nonexistent segments is a startup failure.

Do not infer a checkpoint LSN by guessing. Restore the RDB, manifest, and WAL
set from a consistent backup, or use the normal recovery path on the complete
original set.

## RDB is corrupt

A nonzero startup exit with a message such as:

~~~text
Fatal: Failed to load RDB file (file corrupted or inaccessible).
Aborting startup.
~~~

means the snapshot did not validate. Check:

~~~bash
ls -l /var/lib/minidb/kv/minidb.rdb
file /var/lib/minidb/kv/minidb.rdb
stat /var/lib/minidb/kv/minidb.rdb
sha256sum /var/lib/minidb/kv/minidb.rdb
~~~

The process never starts empty or overwrites a damaged snapshot just because the
RDB cannot be loaded. Common causes include bad magic, a version other than 2, a
checksum mismatch, a truncated record, duplicate keys, invalid varints, and
trailing bytes. Preserve the complete persistence set for diagnosis.

## Recovery exceeds a resource limit

Recovery is intentionally bounded. The hard limits are:

| Resource | Limit |
|---|---:|
| WAL segment size | 16 MiB |
| Total WAL size | 256 MiB |
| WAL segments | 128 |
| Recovered records | 1,000,000 |
| Recovered WAL bytes | 512 MiB |
| RDB records | 100,000 |
| RDB file size | 64 MiB |

A limit breach is a startup error, not a reason to raise the limit blindly.
Check for a runaway workload, a checkpoint that never completed, or an
unexpectedly large WAL directory. Increase a limit only in code, after reviewing
the matching resource-policy decision and adding tests.

## A transaction returns CONFLICT

`CONFLICT=105` is a normal snapshot-isolation write-write conflict, not an I/O
failure. The conflicting transaction ended without allocating an LSN or writing
a WAL record. Re-run the business operation from a new `BEGIN` against a fresh
snapshot; do not retry `COMMIT` on the old transaction handle.

First-committer-wins checks both the latest applied key LSN and accepted pending
commits. Consequently, a conflict can include a write that has not yet appeared
in the latest visible value because its COMMIT is still queued behind group
commit. Check the `transactions.write_conflicts` and
`transactions.conflicted` counters in `STATS`/`/stats`; they are expected to
change under concurrent writers.

## A transaction command reports a state or resource error

One connection can own at most one active transaction. Nested `BEGIN`,
`COMMIT`/`ROLLBACK` without an active transaction, or mutations with an invalid
transaction state return a protocol error without damaging the connection.

Parameter and state errors keep an active transaction open so the caller can
correct the request. Exceeding the 64-active-transaction, 256-write-key, 1-MiB
transaction-payload, or 1,000,000-version limit terminates the affected
transaction; the client must `BEGIN` again. Persistence and allocation failures
are fail-stop conditions rather than retryable transaction errors.

An accepted `COMMIT` still finishes if the client disconnects. An uncommitted
transaction is rolled back on disconnect, session destruction, or shutdown, and
none of its private writes are included in an RDB checkpoint.

## AGENT returns IDEMPOTENCY_CONFLICT

`IDEMPOTENCY_CONFLICT` is an application-level `MDOR` status carried inside a
normal `VALUE` response. The idempotency key already belongs to a different
canonical request. No new state was written.

- To replay the original successful operation byte-for-byte, resend the exact
  original subcommand and argument bytes with the same key.
- To perform a genuinely different operation, use a new idempotency key.
- Wait for the idempotency TTL before reusing the key for different content; the
  default TTL is 24 hours and the maximum is 7 days.
- Do not retry a changed request with the old key in a tight loop.

A transport `CONFLICT=105` is different: it can occur when concurrent operations
race on an idempotency key during admission. Retry the same canonical request
with the same key; the retry should return the stored `MDOR` if the other
operation committed.

## AGENT returns LEASE_HELD, LEASE_EXPIRED, or FENCED

These are deterministic `MDOR` application statuses, not connection failures.

- `LEASE_HELD`: another owner holds a non-expired lease. Do not force a takeover;
  wait for expiry or coordinate with the current owner.
- `LEASE_EXPIRED`: renewal of an already expired lease is forbidden. Acquire a
  new lease and use the resulting higher fence for subsequent tool operations.
- `FENCED`: the supplied fence is lower than the target current last fence. An
  old worker must stop. Re-read the session/tool, obtain the current fence from
  a valid acquisition, and retry only if the current owner policy permits it.
- `VERSION_MISMATCH`: re-read the record and retry with its current revision;
  do not increment expected values blindly.

Recovery intentionally leaves `RUNNING` tools and expired leases unchanged. A
new worker takes over through a new lease/fence; startup never rewrites the old
worker's state.

## AGENT reserved namespace is rejected by ordinary KV

Any key beginning with `\x00minidb-agent-v1\x00` is reserved. Ordinary
binary-KV and HTTP reads/writes reject it, including operations inside explicit
transactions. Use `AGENT=12` with the documented subcommands; there is no HTTP
Agent API and no supported way to access Agent records as ordinary KV keys.

If startup reports a missing/corrupt Agent marker, invalid `MDAR`, namespace
collision, orphan tool, or Agent resource-count mismatch, preserve the complete
RDB/manifest/WAL set for diagnosis. Do not delete the marker or individual Agent
records to force startup: that would turn recoverable corruption into silent
state loss.

## AGENT inside an explicit transaction returns ERROR

`AGENT` cannot be mixed with a `BEGIN` transaction. The error is intentional and
leaves the existing transaction active and unchanged. Finish it with `COMMIT`
or `ROLLBACK`, then issue the Agent command on the same connection.

If an Agent write returns `ERROR` for parameter, resource, shutdown, or I/O
reasons, distinguish a parse/validation failure from persistence fail-stop using
the error text and `agent`/group-commit stats. Invalid parameters do not write
WAL. Persistence I/O failure may mark the process fail-stop and requires repair
and restart.

## Persistence entered fail-stop

`SYNC` requires a WAL append and file fsync before a mutation is acknowledged.
`INTERVAL` requires a successful append and acknowledges before the flush
window closes. If an append or required fsync fails, the persistence layer
marks itself failed and fail-stops rather than reporting false success.

Observed behavior:

- the failing mutation returns `ERROR`;
- later mutations fail closed;
- reads may still reflect the already applied in-memory state;
- `/stats` and KV `STATS` still return a JSON snapshot with `failed=true`;
- `/stats` returns 503 only when the persistence manager was never opened;
- the process must be restarted after the storage problem is repaired.

Check the server log, filesystem errors, free space, and the stats JSON. Do not
clear the error flag in place and continue writing.

## Group-commit admission is full

The writer has fixed admission and completion limits. When admission is full,
MiniDB rejects the mutation before assigning an LSN: HTTP returns a JSON resource
limit error (currently status 413) and KV returns `ERROR` with
`Persistence resource limit exceeded`. A rejected request does not invoke its
completion callback and is not silently queued.

Inspect the top-level `group_commit` object in `/stats` or KV `STATS`:

- `queue_rejections` identifies admission pressure;
- `queue_current_entries`, `queue_max_entries`, and their byte counterparts
  show queue occupancy and high-water marks;
- `completion_backlog_*` shows owner-loop drain pressure;
- `writer_failures`, `append_failures`, `fsync_failures`, and
  `writer_state` distinguish overload from a fail-stop storage error.

Do not raise limits or retry in a tight loop before identifying whether the
bottleneck is the writer, fsync, the owner loop, or a stalled client. A full
completion backlog is a fail-stop condition, not ordinary backpressure.

## Checkpoint stays non-idle or SAVE times out

Inspect the `persistence` object in `/stats` or KV `STATS`:

- `background_checkpoint_state` should return to `idle` after a successful SAVE;
  `capture`, `writing`, `finalizing`, or `cleanup` means a checkpoint is still
  in progress, while `failed` means the persistence path entered fail-stop;
- `checkpoint_admission_blocked_*` isolates the two short admission cuts;
- `checkpoint_snapshot_capture_*`, `checkpoint_snapshot_serialize_*`,
  `checkpoint_snapshot_write_*`, and `checkpoint_snapshot_fsync_*` separate
  capture from worker serialization and durable I/O;
- `checkpoint_manifest_total_us`, `checkpoint_wal_cleanup_total_us`,
  `checkpoint_total_us`, and `checkpoint_failures` identify publication and
  cleanup problems.

A large SAVE latency dominated by `checkpoint_snapshot_serialize_*` does not
mean admission was blocked for that whole interval. Capture remains O(records)
and runs while admission is paused; RDB serialization and durable worker I/O do
not. If cleanup fails after the manifest is durable, the checkpoint itself is
published: preserve WAL residue, allow the next SAVE to retry cleanup, and do
not delete the manifest or segments manually. If `background_checkpoint_state`
stays non-idle or a SAVE never returns, collect process logs and the persistence
set before restarting.

## Save or checkpoint fails during shutdown

The process exits nonzero when the final checkpoint fails. Check directory
permissions and filesystem behavior:

~~~bash
ls -ld /var/lib/minidb/kv
ls -la /var/lib/minidb/kv
df -h /var/lib/minidb/kv
mount | grep minidb
~~~

A temporary `.tmp.*` file means a previous save did not complete; preserve it
for diagnosis and do not treat it as a checkpoint. If rename succeeded but
parent-directory fsync failed, the new RDB may be visible while durability is
unconfirmed. The checkpoint path enters fail-stop and the shutdown exit is
nonzero.

## HTTP returns 400, 413, 414, 431, or 501

These are intentional protocol limits, not necessarily server bugs:

- 400: malformed request line, invalid Host, bad framing, unsupported Upgrade, or an unsupported/ambiguous query;
- 413: body or pending input exceeds the limit;
- 414: request line or target exceeds the limit;
- 431: header bytes or field count exceeds the limit;
- 501: unsupported Transfer-Encoding or a valid extension method.

Check the request against `docs/protocol-http.md`. The parser is strict about
HTTP/1.1 and CRLF; an HTTP/1.0 client or a proxy that changes framing is
rejected deliberately.

## HTTP connection hangs

The server parses one request at a time and resumes buffered requests only after
write completion. A client that sends a partial body or withholds CRLF receives
408 and the connection closes after the idle timeout. A client that reads slowly
can backpressure the server; the output budget is 1.25 MiB per connection.

If the process accepts connections but no test progresses, capture a bounded
strace and inspect the server log. Do not add unbounded sleeps to make tests
pass. For HEAD, read the status and headers, honor `Content-Length` as GET
metadata, and do not wait for body bytes; the connection is ready for the next
response immediately after the header terminator.

## KV returns ERROR and closes

Malformed framing is not resynchronized. Confirm the frame is exactly:

~~~text
frame_len : u32 BE
command   : u8
argc      : u8
arg_len   : u32 BE, repeated
arg       : raw bytes
~~~

`frame_len` excludes the four-byte prefix, includes command and argument
framing, and must consume the whole frame. Unknown command codes and
unrecoverable framing errors close the connection. Command-level argument errors
return `ERROR` while keeping the connection usable; persistence fail-stop errors
are different and require a restart after repair.

## KV TTL looks wrong

- TTL -1 means the key exists with no expiry.
- TTL -2 means missing or expired.
- A non-negative value is remaining milliseconds, rounded down.
- A non-positive EXPIRE removes the key.
- Running deadlines use monotonic time; wall-clock jumps do not change a live
  deadline.
- WAL and RDB records store an absolute Unix millisecond deadline, so restart
  does not reset the countdown.
- SET clears an existing TTL.

The HTTP control plane exposes read-only TTL metadata only through the exact
query `GET/HEAD /kv/<key>?ttl=1`. It does not expose EXPIRE; use the binary KV
protocol to set or change TTL. An empty, repeated, unknown, or differently
valued query returns 400, and a missing or expired key returns 404. For HEAD,
the same JSON length is reported in `Content-Length` but the JSON body is
suppressed.

## Fuzz run produces an artifact

Six fixed-seed Clang libFuzzer targets are part of the acceptance smoke:

~~~text
minidb_http_parser_fuzz
minidb_kv_protocol_fuzz
minidb_wal_record_fuzz
minidb_checkpoint_manifest_fuzz
minidb_agent_record_codec_fuzz
minidb_agent_protocol_fuzz
~~~

Each target runs 20,000 runs with seed `20261111`. A non-empty artifact
directory means the run found a crash. Preserve the file:

~~~bash
find /tmp/minidb-fuzz-artifacts -maxdepth 1 -type f -print
~~~

Minimize with `-minimize_crash=1`, add the input to the matching corpus, add a
durable regression test, and rerun all six targets. The acceptance run is
clean only when every target exits 0, no ASan/UBSan diagnostic appears, and the
artifact directory remains empty.

## perf reports permission or event errors

WSL2 commonly exposes only a subset of software events. Record the attempted
command and the exact error. Use available counters such as task-clock and
page-faults. Do not publish cycles, instructions, branch misses, cache misses, or
flame graphs that the environment did not provide.

## Extracting a diagnostic bundle

Useful non-destructive evidence:

~~~bash
date -u > /tmp/minidb-diagnostics.txt
uname -a >> /tmp/minidb-diagnostics.txt
cmake --version >> /tmp/minidb-diagnostics.txt
c++ --version >> /tmp/minidb-diagnostics.txt
clang++ --version >> /tmp/minidb-diagnostics.txt
git rev-parse HEAD >> /tmp/minidb-diagnostics.txt
git status --short >> /tmp/minidb-diagnostics.txt
ss -ltnp >> /tmp/minidb-diagnostics.txt 2>&1 || true
~~~

For a failing CTest case:

~~~bash
ctest --test-dir build/release --output-on-failure -R 'ExactTestSuite.ExactTestName' -V
~~~

For a server process, use a bounded strace and preserve stdout and stderr. Do
not kill the process with SIGKILL when a graceful SIGTERM is enough; use SIGKILL
only for an intentional crash-recovery experiment.
