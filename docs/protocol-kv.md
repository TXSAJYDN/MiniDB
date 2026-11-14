# MiniDB binary KV protocol

## Scope

The KV server speaks a small binary request/response protocol over TCP. It is
designed for MiniDB's single-node, bounded-resource environment. It is not a
Redis wire-protocol implementation and does not provide a general command
language.

Explicit multi-key transactions, atomic `MSET=13` writes, and the `AGENT=12`
durable Agent-state backend are available on the binary protocol only.
Transactions provide snapshot isolation with first-committer-wins conflict
detection; `MSET` is one atomic multi-key record, not an explicit transaction;
Agent operations use strict revision/CAS, idempotency, and session leases. HTTP
exposes neither a transaction handle nor an Agent API. See
[ADR-008](decisions/008-mvcc-transactions.md) and
[ADR-009](decisions/009-agent-state-backend.md) for the frozen semantics.

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
| `BEGIN` | 9 | 0 | `OK` with one ASCII transaction id |
| `COMMIT` | 10 | 0 | `OK` with one ASCII commit LSN (`0` for a read-only transaction) |
| `ROLLBACK` | 11 | 0 | `OK` with no arguments |
| `AGENT` | 12 | subcommand + subcommand arguments | `VALUE` containing one `MDOR` response; see below |
| `MSET` | 13 | 1..127 `key value` pairs | `OK` with no arguments |

Mutation commands are `SET`, `MSET`, `DEL`, and `EXPIRE`; Agent mutations are
the specific `AGENT` subcommands listed below. A standalone KV mutation is an
implicit one-operation transaction; `MSET` is one atomic multi-key record. Its
response is sent only after the persistence manager accepts the operation
according to the selected durability mode.

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

## Explicit transactions

`BEGIN`, `COMMIT`, and `ROLLBACK` are valid only on a connection that does not
already have an active transaction:

- `BEGIN` with a transaction already active returns `ERROR`; it does not replace
  or commit the existing transaction.
- `COMMIT` or `ROLLBACK` without an active transaction returns `ERROR`.
- `BEGIN` returns a decimal ASCII transaction id. The id is scoped to the
  current connection and server process; it is not a reconnect token.
- Argument, key, or TTL validation errors inside a transaction return `ERROR`
  but keep the transaction active so the client can correct the command.
- Resource-limit and persistence failures end the transaction. The client must
  not assume that an earlier private write is still usable.

Inside a transaction, `SET`, `GET`, `DEL`, `EXPIRE`, and `TTL` operate on the
transaction's private write set and snapshot. `SAVE` and `STATS` are not
transaction-local data operations. A second `ROLLBACK` after a successful
rollback returns `ERROR`.

### Isolation and visibility

MiniDB implements snapshot isolation with first-committer-wins:

1. `BEGIN` records `snapshot_lsn`, the greatest LSN already applied at that
   moment.
2. A read first checks the transaction's private write set, then the newest
   committed version with `commit_lsn <= snapshot_lsn`. This provides
   read-your-writes, repeatable reads for keys not written by the transaction,
   and no dirty reads.
3. A missing or tombstone version is returned as missing/not found. If the
   selected version is expired, the transaction sees a missing key; it never
   falls back to an older version.
4. At `COMMIT`, admission checks every key in the private write set. A conflict
   exists when either the last committed write or an accepted-but-not-yet-applied
   pending commit for that key has an LSN greater than `snapshot_lsn`:

   ~~~text
   max(committed_key_lsn, pending_key_lsn) > snapshot_lsn
   ~~~

   If any key conflicts, the whole transaction returns `CONFLICT` and publishes
   nothing. Otherwise one `kTxn` WAL record represents the complete transaction
   under one LSN and is applied atomically.

This is not serializable isolation. Concurrent transactions can read and write
different keys and produce write skew; there is no predicate, range, or
serializability conflict check. Applications that need serializable behavior
must serialize the relevant decision themselves.

### Commit, rollback, and connection loss

- A successful `COMMIT` for a transaction with at least one write receives one
  new LSN and follows the selected durability mode. In `SYNC` mode the response
  is sent only after the WAL batch containing the transaction record is fsynced
  and the complete transaction has been applied.
- A read-only transaction commits without writing WAL and without allocating an
  LSN. Its response is `OK` with argument `0`.
- `ROLLBACK` discards the private write set, does not write WAL, and does not
  allocate an LSN.
- A write conflict returns `CONFLICT`, discards the transaction, does not write
  WAL, and does not allocate an LSN.
- Once a `COMMIT` is accepted and the response is successful, disconnecting does
  not undo it. If the connection closes before a successful response, the client
  must treat the outcome as unknown and reconcile on reconnect.
- An uncommitted transaction is rolled back when the connection closes, the
  session is destroyed, or the server shuts down. Private writes are never
  included in a checkpoint and are never visible to another connection.

### Transaction resource limits

| Item | Hard limit |
|---|---:|
| Active transactions | 64 |
| Distinct write keys per transaction | 256 |
| Transaction payload | 1 MiB |
| Global version entries | 1,000,000 |
| Value | 1 MiB |
| Key | 1 KiB |

Version garbage collection may retain history needed by active snapshots. If it
cannot respect the hard version budget, the server rejects the operation rather
than silently breaking snapshot reads. Long-running or abandoned snapshots can
therefore cause bounded resource rejections.

### Wire examples

`BEGIN` request and response for transaction id `1`:

~~~text
00 00 00 02 09 00
00 00 00 07 65 01 00 00 00 01 31
~~~

`COMMIT` request and response for LSN `42`:

~~~text
00 00 00 02 0a 00
00 00 00 08 65 01 00 00 00 02 34 32
~~~

Conflict response:

~~~text
00 00 00 02 69 00
~~~


## Atomic `MSET`

`MSET` accepts one or more `key value` pairs. It is an atomic multi-key write
that reuses MiniDB's existing transaction WAL record without creating an
explicit transaction.

### Arguments and limits

| Item | Rule |
|---|---|
| Pair count | 1..127 over the wire |
| Key | 1..1024 bytes |
| Value | 0..1 MiB |
| Raw key + value bytes | At most 1 MiB across the command |
| Encoded `MTXN` payload | At most 1 MiB including transaction/operation headers |
| Duplicate key | Rejected; input is not order-dependent |

The wire limit is **127 pairs**, not 256: the frame has `argc:u8`, so a maximum
of 255 arguments can be represented; `MSET` requires an even number and uses at
most 254 arguments. The persistence API itself can admit up to the existing
256-operation transaction limit for direct non-wire callers, but no protocol
frame can reach that count.

`MSET` is rejected as a whole before WAL admission when any of these checks
fails:

- the argument list is empty or has an odd length;
- a key is empty or longer than 1 KiB;
- a value is larger than 1 MiB;
- the key/value list or encoded `MTXN` payload exceeds its 1 MiB limit;
- a duplicate key occurs;
- a key is in the reserved Agent namespace;
- the command is issued inside an explicit transaction;
- persistence admission or the group-commit queue rejects the record.

### Atomicity and isolation

The server sorts entries by key and encodes them as `SET` operations in one
existing `MTXN` payload carried by one `kTxn` WAL record. The operation receives
exactly one LSN and one completion callback. After recovery, every key from that
record is replayed atomically: either all values are restored or none are.

`MSET` does **not**:

- allocate a transaction id or appear in transaction statistics;
- create a snapshot or provide transaction-local reads;
- run first-committer-wins validation for itself;
- make `MGET` available, or return the values it wrote.

Its admitted keys still participate in the pending-key conflict tracking used
by explicit transactions. Therefore a concurrent transaction whose snapshot
predates the `MSET` can return `CONFLICT` if it later commits a write to one of
those keys. The transaction conflict formula and `kTxn` commit path are
unchanged.

`MSET` is unavailable inside `BEGIN`; it returns `ERROR` and leaves the active
transaction unchanged. To update several keys with snapshot isolation or
read-your-writes, use an explicit transaction instead.

The success response is:

~~~text
OK argc=0
~~~

`SYNC` mode acknowledges only after the WAL record is fsynced and the complete
multi-key apply succeeds. `INTERVAL` mode uses the same bounded flush window as
ordinary KV mutations. A failed admission writes no key; a persistence fail-stop
is never converted into a successful `MSET` response.

## Agent durable state backend

### Command and subcommands

`AGENT=12` carries one ASCII subcommand as its first argument. The remaining
arguments are subcommand-specific. The 15 subcommands are:

~~~text
SESSION_CREATE, SESSION_GET, SESSION_UPDATE, SESSION_CLOSE, SESSION_PURGE
TOOL_CREATE, TOOL_GET, TOOL_CLAIM, TOOL_COMPLETE, TOOL_REQUEUE
LEASE_ACQUIRE, LEASE_RENEW, LEASE_RELEASE
RECORD_GET, RECORD_CAS
~~~

All integers are unsigned decimal ASCII unless an argument is explicitly a
state name. A u64 must be consumed completely by `from_chars`; leading plus
signs, whitespace, and localized forms are invalid. Parameter syntax is:

~~~text
SESSION_CREATE   <session_id> <session_ttl_ms> <state> <idem_key> [idem_ttl_ms]
SESSION_GET      <session_id>
SESSION_UPDATE   <session_id> <expected_revision> <target_state> <session_ttl_ms> <state> <idem_key> [idem_ttl_ms]
SESSION_CLOSE    <session_id> <expected_revision> <final_state> <idem_key> [idem_ttl_ms]
SESSION_PURGE    <session_id> <expected_revision> <idem_key> [idem_ttl_ms]
TOOL_CREATE      <session_id> <tool_id> <tool_name> <request> <idem_key> [idem_ttl_ms]
TOOL_GET         <session_id> <tool_id>
TOOL_CLAIM       <session_id> <tool_id> <expected_tool_revision> <fence> <lease_ttl_ms> <idem_key> [idem_ttl_ms]
TOOL_COMPLETE    <session_id> <tool_id> <expected_tool_revision> <fence> <final_state> <result> <idem_key> [idem_ttl_ms]
TOOL_REQUEUE     <session_id> <tool_id> <expected_tool_revision> <fence> <idem_key> [idem_ttl_ms]
LEASE_ACQUIRE    <session_id> <lease_ttl_ms> <idem_key> [idem_ttl_ms]
LEASE_RENEW      <session_id> <expected_revision> <fence> <lease_ttl_ms> <idem_key> [idem_ttl_ms]
LEASE_RELEASE    <session_id> <expected_revision> <fence> <idem_key> [idem_ttl_ms]
RECORD_GET       <record_name>
RECORD_CAS       <record_name> <expected_revision> <value> <idem_key> [idem_ttl_ms]
~~~

`TOOL_CREATE` creates a tool at revision 0 and bumps its parent session
revision. The expected revision in `TOOL_CLAIM`, `TOOL_COMPLETE`, and
`TOOL_REQUEUE` is the tool revision; the parent-session revision is checked and
advanced internally. A client that also needs to prove parent-session freshness
reads the session and uses the current session revision in its workflow.

Session and tool state names are validated strictly. Session TTL is
1 minute through 30 days, lease TTL is 1 second through 1 hour, and idempotency
TTL is 1 minute through 7 days (default 24 hours). Names are 1..128 bytes;
tool names are at most 256 bytes. The reserved prefix is
`\x00minidb-agent-v1\x00`.

### Mutation idempotency

Every Agent mutation requires an idempotency key of 1..128 binary bytes. The
server hashes a canonical request (subcommand, all semantic fields, and the
idempotency TTL) with SHA-256.

- If the same key has not been seen, the mutation and its idempotency record are
  committed together in one internal `kTxn` at one LSN.
- If the same key has the same canonical request, the previously stored
  `MDOR` is returned exactly. No new WAL record and no new LSN are created.
- If the same key has a different canonical request, the server returns an
  `MDOR` with status `IDEMPOTENCY_CONFLICT` and performs no mutation.
- An idempotency key may be reused after its idempotency TTL expires.
- A disconnect after admission does not cancel the commit. Reconnect and retry
  with the same key. If admission itself races, the transport may return
  `CONFLICT=105`; retry the same canonical request with the same key.

The first operation inside a mutation and its idempotency record become visible
at the same committed LSN, so recovery cannot expose one without the other.

### MDOR response envelope

Every completed `AGENT` application result is returned as one `VALUE`
argument containing:

~~~text
magic[4]          = "MDOR"
version:u8        = 1
status:u8
flags:u8          = 0
reserved:u8       = 0
revision:u64
fence:u64
lsn:u64
payload_len:u32
payload[payload_len]
~~~

The fixed header is 36 bytes. Status values are:

| Code | Name | Meaning |
|---:|---|---|
| 0 | `OK` | Update succeeded |
| 1 | `CREATED` | Create succeeded |
| 2 | `NOT_FOUND` | Target missing or logically expired |
| 3 | `VERSION_MISMATCH` | Expected revision did not match |
| 4 | `ALREADY_EXISTS` | Revision 0 expected but target exists |
| 5 | `LEASE_HELD` | A live lease prevents acquisition |
| 6 | `LEASE_EXPIRED` | The lease expired and cannot be renewed |
| 7 | `FENCED` | Fence is below the current last fence |
| 8 | `INVALID_STATE` | Current state disallows the operation |
| 9 | `IDEMPOTENCY_CONFLICT` | Same key, different canonical request |
| 10 | `RESOURCE_LIMIT` | A single request/field limit was exceeded |
| 11 | `CAPACITY` | A global count or logical-byte budget is exhausted |

Write responses have an empty payload. `SESSION_GET`, `TOOL_GET`, and
`RECORD_GET` return a canonical `MDAR` record or valid read-only view as the
payload. For a write, `revision` is the target's new revision. For a read it is
the returned revision. `fence` is the relevant session fence (zero for generic
records), and `lsn` is the successful mutation LSN. Reads and business
rejections report `lsn=0`.

Deterministic application rejections (for example `VERSION_MISMATCH`,
`LEASE_HELD`, or `FENCED`) are `VALUE` responses carrying an `MDOR`; they
do not close the connection. Transport framing errors, malformed parameters,
unknown subcommands, or persistence unavailability use `ERROR`. Invalid
argument/state errors are detected before any WAL or idempotency write.

### Isolation and transaction interaction

`AGENT` is unavailable inside an explicit `BEGIN` transaction. It returns
`ERROR` and does not alter the active transaction. HTTP has no Agent route and
cannot use an implicit HTTP mutation to bypass this rule.

All ordinary KV/HTTP reads and writes reject any key beginning with
`\x00minidb-agent-v1\x00` before accessing the store. This includes SET, GET,
DEL, EXPIRE, TTL, explicit-transaction reads, and private transaction writes.
Agent state is therefore inaccessible except through `AGENT`.

### Agent statistics

KV `STATS` contains a top-level `agent` object in the same coherent
`PersistenceSnapshot` as the other statistics. Its stable fields are:

~~~text
sessions_stored, sessions_live
sessions_created, sessions_completed, sessions_failed, sessions_cancelled,
sessions_purged
tools_stored, tools_pending, tools_running, tools_succeeded,
tools_failed, tools_cancelled
records_stored, idempotency_stored
logical_bytes, logical_bytes_max
idempotency_replays, idempotency_conflicts
revision_mismatches
lease_acquires, lease_renews, lease_expiries
fenced_rejections
gc_runs, gc_candidates, gc_deleted, gc_failures
gc_total_us, gc_max_us
~~~

`logical_bytes_max` is 64 MiB. `tools_*` count effective states: a
non-terminal tool whose parent session is terminal is counted as cancelled.
Timing counters include successful GC runs only and are diagnostic, not SLOs or
arbitrary latency quantiles. Existing stats objects retain their names and
types.

### Limits

| Resource | Limit |
|---|---:|
| Physical session records | 1,024 |
| Physical tool records | 8,192 |
| Tools per session | 128 |
| Generic records | 16,384 |
| Idempotency records | 8,192 |
| Agent logical bytes | 64 MiB |
| One MDAR record | 64 KiB |
| Session/tool/record name | 128 bytes |
| Tool name | 256 bytes |
| Idempotency key | 128 bytes |
| GC candidate groups per batch | 64 |
| One Agent `kTxn` | 256 key operations |

An Agent mutation uses the normal durability mode: `SYNC` acknowledges only
after the covering batch is fsynced and the owner loop has applied the complete
`kTxn`; `INTERVAL` has the same bounded flush window as ordinary KV.

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
  "transactions": {
    "active": 0,
    "started": 5,
    "committed": 4,
    "conflicted": 0,
    "rolled_back": 1,
    "failed": 0,
    "write_conflicts": 0,
    "max_active": 2,
    "versions_current": 42,
    "versions_max": 84,
    "versions_reclaimed": 0,
    "oldest_snapshot_lag_lsn": 0,
    "max_oldest_snapshot_lag_lsn": 7,
    "commit_records": 3,
    "commit_bytes": 512,
    "commit_total_us": 12000,
    "commit_max_us": 9000,
    "gc_total_us": 0,
    "gc_max_us": 0
  },
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

An active explicit transaction is not part of the checkpoint. Its private write
set is discarded on shutdown or crash, while committed `kTxn` records after the
captured checkpoint LSN are retained by the manifest and replayed on startup.

The top-level `group_commit` object reports admission queue entries/bytes and
rejections, successful batch counts and size distribution, physical data fsyncs,
writer/completion timings, completion backlog high-water marks, failure counters,
and `writer_state`. Batch-size bucket bounds are `1,2,4,8,16,32,64,128`; the
ninth count is the `129..256` overflow bucket.

The top-level `transactions` object reports transaction counts, active and
version high-water marks, snapshot lag, successful transaction-WAL records and
bytes, commit admission-to-apply timings, and GC timings. `active` includes
transactions in the committing window. `versions_current` counts physical
version entries, including tombstones; `oldest_snapshot_lag_lsn` is zero when
there is no active snapshot. These are bounded diagnostic counters, not SLOs or
arbitrary-latency quantiles.

## Response commands

| Name | Code | Meaning |
|---|---:|---|
| `ERROR` | 100 | One argument containing a human-readable error |
| `OK` | 101 | Zero or more arguments, command-specific |
| `VALUE` | 102 | One raw value argument |
| `NOTFOUND` | 103 | No arguments |
| `INTEGER` | 104 | One decimal ASCII integer argument |
| `CONFLICT` | 105 | No arguments; the transaction was discarded without WAL |

Responses use exactly the same frame layout as requests.

## Error and connection behavior

- Invalid argument count, invalid key size/length, invalid TTL text, malformed
  Agent parameters, or `SAVE` failure returns `ERROR` but leaves the connection
  usable unless the failure is a persisted fail-stop condition.
- `AGENT` application outcomes such as `VERSION_MISMATCH`, `LEASE_HELD`, and
  `FENCED` are successful transport exchanges carrying an `MDOR` in a
  `VALUE` response. They are not connection errors.
- `AGENT` or `MSET` inside an explicit transaction returns `ERROR` and
  leaves the transaction active and unchanged.
- Invalid transaction command sequences and argument errors leave an existing
  transaction active. `CONFLICT`, transaction resource exhaustion, or a
  persistence failure ends it.
- A connection close rolls back its uncommitted transaction. Once COMMIT is
  accepted, connection loss does not revoke it.
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

Startup then validates the manifest, loads the RDB checkpoint, replays the WAL
segments referenced by the manifest, and validates the Agent reserved namespace.
The Agent marker and every `MDAR` must decode strictly; indexes and counts are
rebuilt, every tool must have its parent session, and resource limits must
hold. A missing marker, invalid Agent record, namespace collision, or orphan
tool is fatal. A missing referenced segment, a middle-of-file WAL corruption, a
bad checksum, an LSN gap, or a resource-limit breach is also fatal. The process exits nonzero, does not listen, and does not
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
- [ADR-008: MVCC and snapshot-isolation transactions](decisions/008-mvcc-transactions.md)
- [ADR-009: Agent durable state backend](decisions/009-agent-state-backend.md)
