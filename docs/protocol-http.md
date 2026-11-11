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
The query string is retained in the parsed request but is not used for routing.

| Method | Path | Behavior |
|---|---|---|
| GET | `/health` | Returns `OK\n` with 200 |
| GET | `/stats` | Returns persistence statistics JSON with 200 |
| POST | `/echo` | Returns the exact request body as `application/octet-stream` |
| POST | `/save` | Synchronously publishes a durable checkpoint and returns JSON status |
| GET | `/kv/<key>` | Returns the raw value or 404 |
| POST | `/kv/<key>` | Stores the exact body as the value and returns 200 |
| DELETE | `/kv/<key>` | Deletes a live key or returns 404 |

For `/kv/<key>`:

- the path prefix is exactly four bytes: `/kv/`;
- the key is the remaining raw byte sequence of the path;
- the key must be 1..1024 bytes;
- values are binary-safe and may be empty;
- POST clears any existing TTL for the key;
- HTTP does not expose EXPIRE or TTL operations; use the binary protocol for
  those commands.

A known path with the wrong method returns 405 with an `Allow` header. A known
method on an unknown path returns 404. A valid extension method returns 501.

## `/save` semantics

`POST /save` performs a stop-the-world checkpoint:

1. flush and fsync the WAL;
2. rotate the active segment if it contains records;
3. write, fsync, rename, and directory-fsync the RDB snapshot;
4. atomically publish the checkpoint manifest;
5. remove obsolete WAL segments and fsync the WAL directory.

A 200 response means the checkpoint succeeded through manifest publication.
A failure returns 500 and does not claim durability. A directory-fsync failure
after rename may leave the new snapshot visible while durability is
unconfirmed; the server reports that condition and enters fail-stop rather than
publishing an unsafe manifest.

`/save` is not a lightweight backup command. It can pause request handling for
the duration of snapshot serialization, file I/O, fsyncs, and cleanup.

## `/stats` response

`GET /stats` returns `application/json` with a stable top-level shape:

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
    "checkpoint_pause_us": 0
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

All fields come from one `PersistenceSnapshot` taken under the persistence
mutex. Counters are cumulative for the current process unless their names
explicitly describe recovery state. The endpoint is diagnostic, not a
replacement for `/health` or a liveness probe.

`timing` records successful operations only. `wal_append_*` covers
`write_all()` without record encoding, rotation, or fsync. `wal_data_fsync_*`
covers successful WAL data fsyncs, including empty-segment creation, but not
directory fsyncs. `sync_mutation_*` starts after mutation validation and spans
bootstrap, WAL append/fsync, and in-memory apply for successful `SYNC`
mutations; it excludes network parsing and response serialization. `total`
fields support window means, while `max` fields are process-lifetime maxima
that include preload/warm-up. The fixed buckets partition `wal.data_fsyncs`;
they provide bounded cardinality, not arbitrary quantiles or per-request traces.

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
| 400 | Malformed request, invalid Host, invalid method token, unsupported Upgrade, or bad framing |
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
curl -i http://127.0.0.1:8080/stats
~~~

Store and fetch a binary-safe value:

~~~bash
curl -i -X POST --data-binary 'value' http://127.0.0.1:8080/kv/key
curl -i http://127.0.0.1:8080/kv/key
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
