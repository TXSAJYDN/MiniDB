# MiniDB HTTP/1.1 protocol

## Scope

MiniDB implements a deliberately small HTTP/1.1 request/response subset. It is
not a general web server and does not claim RFC-complete HTTP support.

A request must use:

- one request per HTTP message;
- the origin-form request target (a path beginning with `/`);
- exactly **HTTP/1.1**;
- CRLF line endings;
- `Content-Length` for any request body.

The parser rejects HTTP/1.0 and other well-formed versions with 505, malformed
versions with 400, and unsupported transfer framing rather than guessing at a
message boundary.

HTTP does not expose explicit transactions. Every `POST /kv/<key>` and
`DELETE /kv/<key>` is an implicit one-operation transaction; there is no
cross-request transaction handle, snapshot, or rollback token. Use the binary KV
protocol when multi-key atomicity or snapshot isolation is required.

## Durability context

The HTTP server wraps the same persistence engine as the binary KV server. The
`-d sync|interval` command-line option selects the durability mode; the default
is `sync`.

Given `-r <rdb_path>`:

~~~text
<rdb_path>              RDB v2 checkpoint snapshot
<rdb_path>.manifest     checkpoint manifest v1
<rdb_path>.wal/         WAL segment directory
<rdb_path>.lock         advisory single-writer lock
~~~

`SYNC` returns success for a mutation only after WAL append and file fsync
succeed. `INTERVAL` acknowledges after append and flushes on a 100 ms timer,
checkpoint, or graceful shutdown. A persistence I/O failure marks the process
fail-stop; later mutations fail rather than reporting false success. A second
process using the same base path is rejected by the writer lock.

## Wire limits

| Item | Limit |
|---|---:|
| Request line including CRLF | 4 KiB |
| Request target (path plus optional query) | 4 KiB |
| Header section after the request line | 16 KiB total |
| Header fields | 64 |
| Request body | 1 MiB |
| Per-connection pending/input budget | 1.25 MiB |
| Per-connection output budget | 1.25 MiB |
| Concurrent connections per process | 32 |
| Idle/incomplete-request timeout | 30 seconds |

The request-line limit is enforced while a line is still incomplete, so a peer
cannot grow the buffer to 64 KiB or more merely by withholding CRLF. Header
bytes are accumulated across complete and incomplete lines. One connection may
pipeline multiple requests, but the server handles only one request at a time
and preserves response order.

## Request grammar

At a high level:

~~~text
GET /kv/example HTTP/1.1\r\n
Host: 127.0.0.1\r\n
Connection: keep-alive\r\n
\r\n
~~~

The method must be a valid HTTP token. Recognized methods are GET, POST, HEAD,
PUT, and DELETE. A syntactically valid extension method is preserved and returns
501 because no route accepts it.

### Host

A syntactically valid request must contain one non-empty `Host` field. Supported
forms are DNS-style host names, IPv4 literals, bracketed IPv6 literals, and an
optional decimal port 0..65535. Duplicate Host fields are rejected.

### Other headers

- Field names are case-insensitive and are stored lower-case.
- Leading/trailing optional whitespace around a field value is removed.
- Control characters in field values are rejected.
- Obsolete line folding is rejected with 400.
- Duplicate `Content-Length` is accepted only when every numeric value is the
  same; leading zeroes are allowed.
- Duplicate `Transfer-Encoding` and Host fields are rejected.
- `Connection` fields may be repeated and are combined as a token list.
- Complete header lines are subject to both the byte and field-count limits.

### Framing

`Content-Length` syntax is a non-negative decimal integer. Signs, whitespace,
empty values, and non-digits are invalid. A value above 1 MiB yields 413.

The server does not support:

- `Transfer-Encoding` / chunked bodies: 501;
- `Expect: 100-continue`: 417;
- `Upgrade` / WebSocket: 400.

GET and HEAD requests with a nonzero Content-Length are rejected with 400. An
empty POST body and an empty value are valid.

## Routing

Routing uses the decoded-free path component. Percent escapes are not decoded.
The query component is never part of route matching.

| Method | Path | Behavior |
|---|---|---|
| GET, HEAD | `/health` | Returns `OK\n` with 200 |
| GET, HEAD | `/stats` | Returns persistence statistics JSON with 200 |
| POST | `/echo` | Returns the exact request body as `application/octet-stream` |
| POST | `/save` | Starts the durable checkpoint state machine and defers the response until publication completes |
| GET, HEAD | `/kv/<key>` | Returns the raw value, TTL metadata with `?ttl=1`, or 404 |
| POST | `/kv/<key>` | Stores the exact body as the value and returns 200 |
| DELETE | `/kv/<key>` | Deletes a live key or returns 404 |

For `/kv/<key>`:

- the path prefix is exactly four bytes: `/kv/`;
- the key is the remaining raw byte sequence of the path;
- the key must be 1..1024 bytes;
- values are binary-safe and may be empty;
- POST clears any existing TTL for the key;
- POST and DELETE do not accept a query component;
- HTTP does not expose EXPIRE, BEGIN, COMMIT, or ROLLBACK; use the binary
  protocol for those commands;
- each HTTP KV mutation is an independent implicit transaction. Two requests
  cannot be committed atomically together, and an HTTP connection cannot pin a
  snapshot across requests.

A known path with the wrong method returns 405 with an `Allow` header. A known
method on an unknown path returns 404. A valid extension method returns 501.

### Query policy

The only supported query is the exact byte string `ttl=1` on
`GET/HEAD /kv/<key>`. There is no percent-decoding, case folding, whitespace
trimming, duplicate merging, or default parameter.

| Query | Result |
|---|---|
| absent | Normal value retrieval |
| `?ttl=1` | TTL metadata for the key |
| `?` | 400 |
| `?ttl=2` or any other value | 400 |
| repeated, duplicate, or unknown parameters | 400 |
| any query on another route or method | 400 |

An unsupported query is rejected before route-specific handling, so, for
example, `POST /kv/key?ttl=1` and `GET /health?` both return 400.

### HEAD semantics

A HEAD response carries the same status code, response headers, and
`Content-Length` that the corresponding GET would produce, but the server omits
the body bytes. This applies to successful routes and to 4xx/5xx responses,
including keep-alive and pipelined requests. `Content-Length` therefore describes
the would-be GET body, not the zero bytes on the HEAD wire; clients must not wait
for a HEAD body.

### KV TTL metadata

`GET /kv/<key>?ttl=1` returns `application/json` with one of these bodies:

~~~json
{"ttl_ms":12345}
~~~

~~~json
{"ttl_ms":null}
~~~

A non-negative integer is the remaining TTL in milliseconds, rounded down. A
`null` value means the key exists without an expiry. A missing or expired key
returns the normal 404 response. `HEAD /kv/<key>?ttl=1` returns the same status
and headers with no body bytes. The metadata route is read-only; setting TTL
still requires the binary KV `EXPIRE` command.

## `/save` semantics

`POST /save` starts the two-cut background checkpoint state machine:

1. pause mutation admission and issue the writer pause barrier;
2. drain accepted WAL records, return completions to the owner loop, and apply
   them in LSN order;
3. pause the TTL scanner, flush and fsync the WAL, and rotate the active segment
   when it contains records;
4. capture an independent `RdbSnapshot` at `checkpoint_lsn`; this capture is
   O(records) and remains inside the admission cut;
5. resume the writer and mutation admission while a checkpoint worker serializes
   the snapshot, writes and fsyncs a temporary RDB, atomically renames it, and
   directory-fsyncs its parent;
6. pause admission again, drain and apply mutations admitted while the worker was
   running, collect the complete live WAL segment set beginning at
   `checkpoint_lsn + 1`, and atomically publish and directory-fsync the manifest;
7. best-effort remove WAL segments preceding the retained boundary and fsync the
   WAL directory, then resume the writer, or finish shutdown.

The HTTP response is deferred until the state machine completes. A 200 response
means the RDB and manifest were durably published. A failure returns 500 and
does not claim durability. A directory-fsync failure after rename may leave the
new snapshot visible while durability is unconfirmed; the server reports that
condition and enters fail-stop rather than publishing an unsafe manifest.

`/save` is not a lightweight backup command. Snapshot capture and the two
admission cuts can still pause new mutations, and capture cost grows with the
number of records. RDB serialization, temporary-file I/O, data fsync, rename,
and directory fsync run on the checkpoint worker rather than the owner loop.
Cleanup failure after a durable manifest does not invalidate the published
checkpoint: residue is safe and a later SAVE retries cleanup.

## `/stats` response

`GET /stats` and `HEAD /stats` return `application/json` with a stable top-level shape:

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

All fields come from one `PersistenceSnapshot`. Queue, completion backlog, WAL
bytes, and writer state are captured as one coherent writer snapshot. Counters
are cumulative for the current process unless their names explicitly describe
recovery state. The endpoint is diagnostic, not a replacement for `/health` or
a liveness probe.

`timing` records successful operations only. `wal_append_*` covers
`write_all()` without record encoding, rotation, or fsync. `wal_data_fsync_*`
covers successful WAL data fsyncs, including empty-segment creation, but not
directory fsyncs. `sync_mutation_*` starts after mutation validation and spans
bootstrap, WAL append/fsync, and in-memory apply for successful `SYNC`
mutations; it excludes network parsing and response serialization. `total`
fields support window means, while `max` fields are process-lifetime maxima
that include preload/warm-up. The fixed buckets partition `wal.data_fsyncs`;
they provide bounded cardinality, not arbitrary quantiles or per-request traces.

Checkpoint timing fields distinguish the short admission cuts from worker I/O.
`checkpoint_admission_blocked_*` covers both pause intervals, including capture
but excluding RDB serialization and durable worker I/O. `checkpoint_snapshot_*`
reports capture, serialization, temporary-file write, and RDB fsync separately.
`checkpoint_manifest_total_us` and `checkpoint_wal_cleanup_total_us` are
cumulative; `checkpoint_total_us` and `checkpoint_max_us` measure successful
checkpoint duration from the first cut to completion. `checkpoint_failures`
counts failed attempts. `save_client_latency_*` measures non-shutdown SAVE
requests. `background_checkpoint_state` is one of `idle`, `capture`, `writing`,
`finalizing`, `cleanup`, or `failed`. Phase counters use monotonic time and need
not sum exactly to end-to-end latency because scheduling and completion
delivery are included in the latter.

The top-level `group_commit` object reports:

- current/max admission queue entries and bytes, plus queue rejections;
- successful batches, records, encoded bytes, maximum batch size, and size
  buckets (bounds `1,2,4,8,16,32,64,128`; index 8 is `129..256` overflow);
- physical WAL data fsyncs and writer-loop, submit-to-fsync, and fsync-to-apply
  timings;
- completion backlog high-water marks;
- `writer_failures`, `append_failures`, `fsync_failures`, and `writer_state`
  (`disabled`, `running`, `paused`, `draining`, `stopped`, or `failed`).

The top-level `transactions` object reports binary-KV explicit transaction
counts, active/version high-water marks, snapshot lag, transaction-WAL records
and bytes, commit timings, and GC timings. HTTP requests cannot create explicit
transactions, but a KV client connected to the same persistence manager may
contribute to these counters. `active` includes transactions in the committing
window. The fields are diagnostic counters, not SLOs.

If the persistence manager was never opened, `/stats` returns 503 with the
plain-text body `Persistence is not open\n`. Once a manager exists and has
entered fail-stop, `/stats` still returns 200 JSON with `failed=true`; use that
flag and the server logs for diagnosis.

## Responses

Every response includes `Content-Length` and either `Connection: Keep-Alive` or
`Connection: close`. There is no Date or Server header. HTTP/1.1 connections
persist unless the request contains the `close` connection token or an error
requires connection termination.

Route-level 404/405 responses normally preserve keep-alive. Parse/validation
errors are final: the server writes the error response and shuts down the
connection.

| Status | Meaning in MiniDB |
|---:|---|
| 200 | Successful route |
| 400 | Malformed request, invalid Host, invalid method token, unsupported Upgrade, bad framing, or unsupported/ambiguous query |
| 404 | Unknown path or missing KV key |
| 405 | Known path, unsupported method |
| 408 | Idle/incomplete request timeout |
| 413 | Body or pending input exceeds limit |
| 414 | Request line/target exceeds limit |
| 417 | Expect header present |
| 431 | Header bytes/fields exceed limit |
| 500 | Internal handler, mutation, or checkpoint failure |
| 501 | Valid extension method, Transfer-Encoding, or no handler |
| 503 | Persistence manager was not opened; fail-stop is reported as 200 JSON with `failed=true` |
| 505 | Well-formed HTTP version other than 1.1 |

## Examples

Health and diagnostics:

~~~bash
curl -i http://127.0.0.1:8080/health
curl -I http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/stats
~~~

Store, fetch, and inspect TTL metadata for a binary-safe value:

~~~bash
curl -i -X POST --data-binary 'value' http://127.0.0.1:8080/kv/key
curl -i http://127.0.0.1:8080/kv/key
curl -I http://127.0.0.1:8080/kv/key
curl -i 'http://127.0.0.1:8080/kv/key?ttl=1'
curl -I 'http://127.0.0.1:8080/kv/key?ttl=1'
~~~

Delete:

~~~bash
curl -i -X DELETE http://127.0.0.1:8080/kv/key
~~~

Explicit checkpoint:

~~~bash
curl -i -X POST http://127.0.0.1:8080/save
~~~

The process also checkpoints on graceful SIGINT/SIGTERM shutdown. A successful
`/save` response means the checkpoint and manifest were published; it does not
prove that a later power loss cannot lose data under a non-conforming device.
