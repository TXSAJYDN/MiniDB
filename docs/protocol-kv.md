# MiniDB binary KV protocol

## Scope

The KV server speaks a small binary request/response protocol over TCP. It is
designed for MiniDB's single-node, bounded-resource environment. It is not a
Redis wire-protocol implementation and does not provide a general command
language.

## Transport

The KV server accepts TCP connections, normally on port 6379. The protocol is
binary-safe: keys and values may contain NUL bytes and arbitrary octets. They
are not interpreted as UTF-8 or C strings.

All fixed-width integers are unsigned big-endian unless explicitly stated
otherwise. A connection is persistent and may carry multiple commands. The
server processes one command at a time per connection and preserves response
order.

## Frame format

~~~text
frame_len : u32 BE
command   : u8
argc      : u8
repeat argc times:
    arg_len : u32 BE
    arg     : arg_len raw bytes
~~~

`frame_len` covers `command`, `argc`, and every argument length/data pair. It
does not include the four-byte `frame_len` prefix itself. The declared frame
must be consumed exactly; trailing bytes inside a frame and truncated arguments
are malformed.

## Limits

| Item | Limit |
|---|---:|
| Single argument | 1 MiB |
| Frame payload | 1 MiB + 64 KiB |
| Arguments per command | 255 (`u8` range) |
| Key length for key commands | 1..1024 bytes |
| Connection pending/input budget | 1.25 MiB |
| Concurrent connections per process | 32 |

Empty values are valid. Empty keys are rejected by key commands. The maximum
frame budget includes framing overhead and allows a 1 KiB key plus a 1 MiB
value.

## Request commands

| Name | Code | Arguments | Success response |
|---|---:|---|---|
| `PING` | 1 | 0 | `OK` with one argument `PONG` |
| `SET` | 2 | key, value | `OK` with no arguments |
| `GET` | 3 | key | `VALUE` with one argument, or `NOTFOUND` |
| `DEL` | 4 | key | `INTEGER` `1` if removed, else `0` |
| `EXPIRE` | 5 | key, ttl_ms | `OK` with no arguments |
| `TTL` | 6 | key | `INTEGER`, semantics below |
| `SAVE` | 7 | 0 | `OK` if checkpoint publication is durable, else `ERROR` |
| `STATS` | 8 | 0 | `VALUE` with one JSON argument |

Mutation commands are `SET`, `DEL`, and `EXPIRE`. Their response is sent only
after the persistence manager accepts the mutation according to the selected
durability mode.

## Durability modes

The server is started with `-d sync` or `-d interval`; the default is `sync`.
The mode changes when a successful mutation response is sent, not the wire
format of the response.

| Mode | Response timing | Acknowledged-write boundary |
|---|---|---|
| `SYNC` | After the WAL batch covering the record completes data fsync, then in-memory apply succeeds | Under the tested file/device model, every acknowledged mutation is recoverable after a process crash |
| `INTERVAL` | After WAL append and in-memory apply succeed | WAL data is flushed by a 100 ms owner-loop timer, before a checkpoint, and before graceful shutdown; recently acknowledged mutations may be lost |

`INTERVAL` is not a promise that every acknowledged write survives a process
crash. Group commit is an internal `SYNC` optimization, not a public
`BATCHED` durability mode. Durability claims cover process crashes and
restarts, not power loss, kernel crashes, firmware cache violations, or media
failure.

If a WAL append or required fsync fails, the persistence layer enters
fail-stop. The operation returns `ERROR`, later mutations also fail, and the
process does not pretend that an unpersisted mutation succeeded.

## `SET`

Stores or replaces the value and clears any existing TTL. The value may be
empty. A successful response is:

~~~text
OK argc=0
~~~

## `GET`

Returns:

- `VALUE` with one raw-byte argument when a live key exists;
- `NOTFOUND` when the key is missing or expired.

`GET` is a read and does not write a WAL record.

## `DEL`

Returns `INTEGER 1` only when a live key was removed. Missing or expired keys
return `INTEGER 0`. The command's WAL append and durability behavior still
follow the selected durability mode when a live key is removed.

## `EXPIRE`

The second argument is a base-10 signed 64-bit integer in milliseconds parsed
with `std::from_chars`. Signs are accepted. A non-positive TTL removes a live
key. The protocol returns `OK` whether or not the key existed; the response does
not expose an existence bit.

TTL deadlines use the process monotonic clock while the process is running.
Wall-clock jumps do not change a live deadline. Persisted records and WAL
records store an absolute Unix epoch millisecond deadline so a restart does not
restart the TTL countdown.

## `TTL`

Returns an `INTEGER` argument:

| Value | Meaning |
|---:|---|
| `-1` | key exists and has no expiry |
| `-2` | key is missing or expired |
| non-negative integer | remaining lifetime in milliseconds, rounded down |

## `SAVE`

Runs the same two-cut background checkpoint state machine as HTTP `POST /save`:

1. pause mutation admission and issue the writer pause barrier;
2. drain accepted records, return completions to the owner loop, and apply them
   in LSN order;
3. pause the TTL scanner, flush and fsync the WAL, and rotate the active segment
   when required;
4. capture an independent `RdbSnapshot` at `checkpoint_lsn`;
5. resume the writer and admission while the worker serializes the snapshot,
   writes and fsyncs a temporary RDB, atomically renames it, and directory-fsyncs
   its parent;
6. pause admission again, drain and apply mutations admitted during worker I/O,
   publish and directory-fsync a manifest containing the complete live WAL
   segment set beginning at `checkpoint_lsn + 1`;
7. best-effort clean up WAL segments before the retained boundary and fsync the
   WAL directory, then resume the writer or finish shutdown.

The `OK` response is deferred until the state machine completes and means the
checkpoint and manifest were durably published. A rename that succeeded
followed by a directory-fsync failure is reported as `ERROR` with
`RDB durability unconfirmed`; the new path may already be visible. The process
enters fail-stop rather than publishing an unsafe manifest. Capture is
O(records) and runs while admission is paused; serialization, temporary-file
I/O, and RDB fsync run on the checkpoint worker. Cleanup failure after a durable
manifest is safe residue and is retried by a later SAVE.

## `STATS`

Returns one `VALUE` argument containing a JSON object. The object has the same
shape as the HTTP `/stats` response and is produced from one coherent
`PersistenceSnapshot`:

~~~json
{
  "durability_mode": "sync",
  "opened": true,
  "failed": false,
  "last_lsn": 42,
  "next_lsn": 43,
  "wal_bytes": 4096,
  "wal_segment_count": 1,
  "persistence": {
    "mutations": 42,
    "sync_mutations": 42,
    "interval_mutations": 0,
    "checkpoints": 0,
    "interval_flushes": 0,
    "checkpoint_pause_us": 0,
    "checkpoint_admission_blocked_total_us": 0,
    "checkpoint_admission_blocked_max_us": 0,
    "checkpoint_snapshot_capture_total_us": 0,
    "checkpoint_snapshot_capture_max_us": 0,
    "checkpoint_snapshot_serialize_total_us": 0,
    "checkpoint_snapshot_serialize_max_us": 0,
    "checkpoint_snapshot_write_total_us": 0,
    "checkpoint_snapshot_write_max_us": 0,
    "checkpoint_snapshot_fsync_total_us": 0,
    "checkpoint_snapshot_fsync_max_us": 0,
    "checkpoint_manifest_total_us": 0,
    "checkpoint_wal_cleanup_total_us": 0,
    "checkpoint_total_us": 0,
    "checkpoint_max_us": 0,
    "checkpoint_failures": 0,
    "background_checkpoint_state": "idle",
    "save_client_latency_total_us": 0,
    "save_client_latency_max_us": 0
  },
  "wal": {
    "appends": 42,
    "bytes_written": 4096,
    "data_fsyncs": 42,
    "directory_fsyncs": 1,
    "rotations": 0,
    "segments_removed": 0
  },
  "timing": {
    "wal_append_total_us": 2100,
    "wal_append_max_us": 120,
    "wal_data_fsync_total_us": 30000,
    "wal_data_fsync_max_us": 8000,
    "sync_mutation_total_us": 45000,
    "sync_mutation_max_us": 12000,
    "fsync_latency_bucket_bounds_us": [
      50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000
    ],
    "fsync_latency_bucket_counts": [20, 10, 5, 3, 2, 1, 1, 0, 0, 0, 0, 0]
  },
  "group_commit": {
    "queue_current_entries": 0,
    "queue_max_entries": 18,
    "queue_current_bytes": 0,
    "queue_max_bytes": 4608,
    "queue_rejections": 0,
    "batches": 12,
    "batch_records": 42,
    "batch_bytes": 21504,
    "max_batch_records": 8,
    "max_batch_bytes": 4096,
    "batch_size_bucket_bounds": [1, 2, 4, 8, 16, 32, 64, 128],
    "batch_size_bucket_counts": [2, 2, 2, 6, 0, 0, 0, 0, 0],
    "data_fsyncs": 12,
    "writer_loop_total_us": 36000,
    "writer_loop_max_us": 4200,
    "submit_to_fsync_total_us": 30000,
    "submit_to_fsync_max_us": 3900,
    "fsync_to_apply_total_us": 9000,
    "fsync_to_apply_max_us": 1200,
    "completion_backlog_current": 0,
    "completion_backlog_max": 8,
    "completion_backlog_current_bytes": 0,
    "completion_backlog_max_bytes": 4096,
    "writer_failures": 0,
    "append_failures": 0,
    "fsync_failures": 0,
    "writer_state": "running"
  },
  "recovery": {
    "rdb_records": 0,
    "checkpoint_lsn": 0,
    "last_lsn": 42,
    "next_lsn": 43,
    "wal_records_scanned": 42,
    "wal_records_replayed": 42,
    "wal_bytes": 4096,
    "truncated_bytes": 0,
    "wal_segments": 1,
    "ignored_residue_segments": 0,
    "legacy_import": false,
    "needs_bootstrap": false,
    "manifest_present": true
  }
}
~~~

The response is diagnostic. It is not a liveness probe or a substitute for
checking process health. If the persistence manager is not initialized,
`STATS` returns `ERROR` with `STATS is unavailable`.

`timing` records successful operations only. `wal_append_*` covers
`write_all()` without record encoding, rotation, or fsync. `wal_data_fsync_*`
covers successful WAL data fsyncs, including empty-segment creation, but not
directory fsyncs. `sync_mutation_*` starts after mutation validation and spans
bootstrap, WAL append/fsync, and in-memory apply for successful `SYNC`
mutations; it excludes network parsing and response serialization. `total`
fields support window means, while `max` fields are process-lifetime maxima
that include preload/warm-up. The fixed buckets partition `wal.data_fsyncs`;
they provide bounded cardinality, not arbitrary quantiles or per-request traces.

Checkpoint timing fields distinguish the two admission cuts from worker I/O.
`checkpoint_admission_blocked_*` covers pause/drain/capture intervals and does
not include RDB serialization or durable worker I/O. The snapshot fields report
capture, serialization, temporary-file write, and RDB fsync separately.
`checkpoint_manifest_total_us` and `checkpoint_wal_cleanup_total_us` are
cumulative. `checkpoint_total_us` and `checkpoint_max_us` measure successful
checkpoint duration; `checkpoint_failures` counts failed attempts.
`save_client_latency_*` covers non-shutdown SAVE requests.
`background_checkpoint_state` is `idle`, `capture`, `writing`, `finalizing`,
`cleanup`, or `failed`. Phase timers are diagnostic and need not sum exactly to
end-to-end latency. A SAVE response arrives only after manifest publication;
cleanup failure after that point leaves safe residue and does not erase the
published checkpoint.

The top-level `group_commit` object reports admission queue entries/bytes and
rejections, successful batch counts and size distribution, physical data fsyncs,
writer/completion timings, completion backlog high-water marks, failure counters,
and `writer_state`. Batch-size bucket bounds are `1,2,4,8,16,32,64,128`; the
ninth count is the `129..256` overflow bucket.

## Response commands

| Name | Code | Meaning |
|---|---:|---|
| `ERROR` | 100 | One argument containing a human-readable error |
| `OK` | 101 | Zero or more arguments, command-specific |
| `VALUE` | 102 | One raw value argument |
| `NOTFOUND` | 103 | No arguments |
| `INTEGER` | 104 | One decimal ASCII integer argument |

Responses use exactly the same frame layout as requests.

## Error and connection behavior

- Invalid argument count, invalid key size/length, invalid TTL text, or `SAVE`
  failure returns `ERROR` but leaves the connection usable unless the failure is
  a persisted fail-stop condition.
- An unknown command code returns `ERROR` and closes the connection.
- A malformed frame, oversized declared frame, truncated final frame that
  exceeds the pending budget, or response that cannot be represented within the
  wire limit returns `ERROR` where possible and closes the connection.
- A persistence write failure returns `ERROR`; subsequent mutations fail
  fail-closed. Reads may still reflect the already applied in-memory state, but
  no further success is claimed without persistence.
- The server does not silently resynchronize after framing corruption.
- There is no KV idle timeout and no authentication/TLS layer.

## Startup, recovery, and ownership

Before listening, the process acquires an exclusive non-blocking `flock` on
`<rdb_path>.lock`. A second process using the same base path exits nonzero
without opening another writable image.

Startup then validates the manifest, loads the RDB checkpoint, and replays the
WAL segments referenced by the manifest. A missing referenced segment, a
middle-of-file WAL corruption, a bad checksum, an LSN gap, or a resource-limit
breach is fatal. The process exits nonzero, does not listen, and does not
overwrite the damaged image. Only an incomplete record at the tail of the final
WAL segment may be truncated and fsynced before recovery.

A startup recovery or lock failure is not converted into an empty database.
Preserve the data set for diagnosis and restore a known-good complete backup.

## Examples

`PING` request:

~~~text
00 00 00 02 01 00
~~~

`PING` response:

~~~text
00 00 00 0a 65 01 00 00 00 04 50 4f 4e 47
~~~

`SET` key `k` value `v` request:

~~~text
00 00 00 0c 02 02
00 00 00 01 6b
00 00 00 01 76
~~~

`SET` response:

~~~text
00 00 00 02 65 00
~~~

`GET` key `k` request:

~~~text
00 00 00 08 03 01 00 00 00 01 6b
~~~

`GET` existing key response:

~~~text
00 00 00 0a 66 01 00 00 00 01 76
~~~

`GET` missing key response:

~~~text
00 00 00 02 67 00
~~~

`STATS` request:

~~~text
00 00 00 02 08 00
~~~

`STATS` response is a `VALUE` frame whose single argument is the JSON payload.
Its exact length depends on the counters at the time of the request.

The Python client `test_kv_client.py` is a runnable smoke example:

~~~bash
python3 test_kv_client.py 6379
~~~

## Related documents

- [Durability modes](decisions/003-durability-modes.md)
- [WAL and manifest format](wal-format.md)
- [Recovery model](recovery.md)
- [Crash consistency](crash-consistency.md)
- [HTTP protocol](protocol-http.md)
