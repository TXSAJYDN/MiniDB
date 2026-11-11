# MiniDB architecture

## Scope

MiniDB is a **crash-consistent single-node key/value store for bounded-resource
environments**. It owns its event loop, non-blocking TCP layer, HTTP and binary
protocol parsers, TTL store, WAL, RDB checkpoint format, checkpoint manifest,
and startup recovery path.

The project is a reliability engineering prototype, not a production database.
It does not claim SQL or Redis compatibility, high concurrency, replication,
transactions, or a production SLA.

Two independent server processes expose the same storage engine:

- **minidb_http_server** exposes health, echo, save, stats, and key/value routes
  over a deliberately strict HTTP/1.1 subset.
- **minidb_kv_server** exposes CRUD, TTL, save, and stats commands over a
  binary-safe framed TCP protocol.

Both processes link the same network and storage libraries. They do not share
memory and must use distinct persistence base paths. A second process is
rejected by an advisory `flock` on `<rdb_path>.lock`.

## Design invariants

1. One owner EventLoop is the only mutation admission, in-memory apply,
   completion, and protocol-response point.
2. A dedicated writer thread exclusively owns WAL append, fsync, rotation, and
   cleanup once group commit starts.
3. A mutation is appended to the WAL before it is applied to the in-memory
   store and before success is reported.
4. `SYNC` reports success only after the physical WAL batch covering its LSN
   has completed the data `fsync`.
5. `INTERVAL` reports success after append and relies on a 100 ms timer,
   checkpoint, or graceful shutdown to flush.
6. RDB is a checkpoint, not the per-write durability mechanism.
7. A checkpoint publishes the snapshot, then the manifest, then removes old WAL
   segments, in that order.
8. A manifest never covers an LSN that the owner has not yet applied.
9. Recovery builds a temporary store and publishes it only after the RDB,
   manifest, and WAL chain all validate.
10. Resource use is bounded by explicit connection, record, segment, recovery,
    snapshot, admission-queue, batch, and completion limits.

## Process and thread model

Each server process creates one EventLoop on the main thread. Socket readiness,
timers, signal handling, protocol parsing, command execution, and checkpoint
initiation all run on that owner thread. This removes data races inside the
connection state machine and makes callback ordering deterministic.

Each `KvStore` may run one background TTL scanner. The scanner holds the same
store mutex as commands, removes expired entries, and does not touch socket
callbacks. It is paused and joined during checkpoint and shutdown so snapshot
publication cannot overlap a background mutation.

The persistence manager coordinates one dedicated `GroupCommitWriter` thread for
server write paths. The owner loop allocates strictly increasing LSNs, validates
mutations, submits admission entries, and is the only thread that applies store
mutations and invokes protocol completions. The writer consumes a contiguous LSN
prefix, encodes a physical batch, appends it to the WAL, and performs one data
fsync for the batch tail. It never touches `KvStore`, sessions, sockets, or user
callbacks.

Completion results return through one coalesced owner-loop drain callback. A
backlog entry is not turned into an individual cross-thread callback. Accepted
requests retain exactly-once completion semantics on success, append failure,
fsync failure, queue limit, writer stop, and shutdown. The writer is not allowed
to run callbacks inline.

Admission queue, physical batch, and completion backlog are bounded:

| Resource | Hard limit |
|---|---:|
| admission queue | 4,096 entries / 64 MiB |
| physical batch | 256 records / 16 MiB |
| completion backlog | 4,096 entries / 64 MiB |

SAVE, checkpoint, interval flush, and shutdown issue writer barriers. Checkpoint
pauses admission, drains and applies all accepted records before rotation,
snapshot, manifest publication, and cleanup. A manifest therefore never includes
an LSN that has not been applied. Shutdown failure is propagated to a nonzero
process exit code.

## Component map

| Layer | Main files | Responsibility |
|---|---|---|
| Base | src/base | RAII descriptors, Buffer, clocks, error values, logging |
| Network | src/net | eventfd/timerfd/signalfd EventLoop, level-triggered epoll, Acceptor, TcpConnection, TcpServer |
| Protocol | src/protocol | HTTP parser/session/routes and binary KV framing/session |
| Storage | src/storage | thread-safe KvStore, RDB checkpoint, WAL, manifest, recovery, single-writer lock |
| Applications | app | CLI parsing, startup validation, signal-driven graceful shutdown |

The dependency direction is approximately:

~~~text
app
 |-- protocol
 |     |-- net
 |     -- storage
 |-- net
 -- storage
~~~

Storage does not depend on the network or protocol layers.

## EventLoop

EventLoop wraps epoll and three dedicated descriptors:

- an eventfd for cross-thread wakeups and queued callbacks;
- a timerfd for one-shot and repeating timers on CLOCK_MONOTONIC;
- an optional signalfd installed by quit_on_signals() for SIGINT and SIGTERM.

The poll loop uses level-triggered epoll. There is no EPOLLET dependency. Active
channels are dispatched only after epoll_wait returns, and callbacks are expected
to run on the owner thread.

Important invariants:

- queue_in_loop() may be called from another thread; the callback itself runs on
  the owner thread.
- Channel registration and socket callbacks are owner-loop operations.
- A connection is not destroyed from inside its own Channel event callback.
  Removal is posted back to the loop so the active callback stack can unwind.
- EventLoop teardown is explicit; the server's network object is destroyed
  before the session map and storage objects that its close callbacks reference.

In `INTERVAL` mode the owner loop also runs a 100 ms repeating timer that calls
`PersistenceManager::flush_interval()`.

## TCP layer

Acceptor owns the listening socket and accepts at most a bounded batch per
readiness event. It handles EINTR, EAGAIN, EMFILE, and ENFILE. An idle /dev/null
descriptor provides the usual EMFILE recovery technique, and acceptance is
paused for a bounded interval under descriptor exhaustion.

TcpConnection owns:

- a non-blocking socket and Channel;
- bounded input and output Buffer objects;
- read, write, close, error, and write-complete callbacks;
- short-write queuing and MSG_NOSIGNAL sends;
- peer EOF, reset, and EPIPE handling.

The per-connection input and output budgets are each 1.25 MiB. A single read is
capped at 64 KiB. The server caps concurrent accepted connections at 32. When
the input budget is exhausted, the protocol session fails closed rather than
growing memory without bound.

start_reading() re-drives bytes that were already read into the TcpConnection
input buffer but withheld by session backpressure. This matters under
level-triggered epoll because no new kernel edge exists for user-space bytes.

## HTTP request path

1. TcpServer accepts a connection and creates an HttpSession on the owner loop.
2. HttpSession appends new bytes to a bounded pending buffer and schedules a
   queue_in_loop() drain. Parsing is never recursively re-entered from a send
   completion.
3. HttpParser parses exactly one HTTP/1.1 request at the current buffer offset.
4. Session validation checks Host, transfer framing, Expect, Upgrade, and the
   complete request-target length.
5. HttpServer routes the request to health, echo, stats, save, or /kv/<key>
   logic.
6. For mutations, the route calls `PersistenceManager`; the in-memory value is
   visible only after the WAL append path succeeds.
7. HttpResponse serializes the status line, headers, and body.
8. The session stops reading while a response is in flight. Write completion
   resumes draining buffered pipelined requests or re-enables socket reads.

`GET /stats` returns the stable persistence statistics JSON. Its `timing`
object reports successful-operation cumulative and process-lifetime maximum
durations plus fixed WAL data-fsync buckets; it is diagnostic evidence for
[ADR-005](decisions/005-group-commit.md), not an SLO. `POST /save` performs a
stop-the-world checkpoint and reports failure or unconfirmed durability
explicitly.

## Idle timeout

An incomplete or idle HTTP request gets a 30-second owner-loop timer. New bytes
restart the incomplete-request interval. The timeout sends 408 and closes the
connection. The KV protocol has no idle timeout.

## Binary KV request path

1. KvServer creates one KvSession per connection.
2. KvSession appends bounded input, then schedules an owner-loop drain.
3. KvProtocol::parse_command() consumes exactly one frame at a time and returns
   incomplete, malformed, or complete without resynchronizing.
4. KvSession validates argument counts, key sizes, and TTL text, then calls
   `PersistenceManager` for mutations and `KvStore` for reads.
5. The response is encoded in the same frame layout and queued through
   TcpConnection.
6. The connection remains usable for command-level errors; framing corruption
   or an unknown command returns ERROR where possible and closes the connection.

`STATS` (command code 8) returns the same persistence statistics JSON as the
HTTP endpoint. `SAVE` (command code 7) performs a checkpoint.

One command is in flight per connection. Pipelined frames remain buffered and
are resumed from write completion, not recursive callbacks.

## Storage and TTL semantics

KvStore stores values in `std::unordered_map` under one mutex. TTL deadlines use
`std::chrono::steady_clock` while the process is live. The WAL and RDB boundary
converts those deadlines to absolute Unix epoch milliseconds, so restarting the
process cannot extend a TTL.

- SET replaces a value and clears any TTL.
- A non-positive EXPIRE removes the key.
- TTL reports -1 for an existing key with no expiry, -2 for a missing or expired
  key, and a non-negative remaining duration otherwise.
- Expired entries are removed lazily and by the optional periodic scanner.
- An EXPIRE mutation is logged as an absolute deadline before it is applied.

The exact wire rules are in [protocol-kv.md](protocol-kv.md).

## Persistence architecture

Given `-r <rdb_path>`:

~~~text
<rdb_path>              RDB v2 checkpoint snapshot
<rdb_path>.manifest     checkpoint manifest v1
<rdb_path>.wal/         WAL segments
<rdb_path>.lock         advisory single-writer lock
~~~

WAL segment names use a 20-digit decimal start LSN:

~~~text
wal-00000000000000000001.log
~~~

### Write path

`PersistenceManager::set`, `del`, and `expire` validate the operation, create a
logical WAL record with the next LSN, and call `WalManager::append`.

- `SYNC`: append and `fsync` the WAL file, then apply the mutation to KvStore,
  then return success.
- `INTERVAL`: append to the file, apply the mutation, and return success. The
  owner-loop timer, checkpoint, or shutdown flushes and fsyncs the WAL.
- If the WAL append or required fsync fails, the persistence layer enters
  fail-stop: later mutations return an error until restart. LSN, byte counters,
  and statistics are not rolled back.
- A complete record that reached the page cache before an fsync failure may
  reappear after recovery. Unacknowledged writes are therefore allowed; an
  acknowledged `SYNC` write is not allowed to disappear.

### Checkpoint ordering

A checkpoint is stop-the-world:

1. pause the TTL scanner;
2. flush and fsync the WAL;
3. rotate the active segment if it contains records, so the next segment starts
   at `checkpoint_lsn + 1`;
4. write the RDB snapshot to a temporary file, fsync it, rename it, and fsync
   the parent directory;
5. atomically write and fsync the manifest naming `checkpoint_lsn` and the
   first retained segment;
6. only then remove obsolete WAL segments and fsync the WAL directory.

A snapshot-durability or manifest-durability failure marks persistence
fail-stop. Old WAL is retained in every ambiguous case. Cleanup failure does not
invalidate a checkpoint: obsolete segments become residue, recovery ignores
segments before the first retained start, and a later checkpoint retries
cleanup.

### Recovery

Startup runs before the listening socket is opened:

1. acquire `<rdb_path>.lock` with `flock(LOCK_EX | LOCK_NB)`;
2. if the manifest is missing:
   - no RDB and no WAL: start empty and bootstrap on the first mutation or
     checkpoint;
   - RDB present and no WAL: import the legacy RDB, then bootstrap on the first
     mutation;
   - WAL present without a manifest: refuse the ambiguous migration;
3. load and validate the RDB into a temporary KvStore;
4. validate the manifest and each referenced segment;
5. scan from the first retained start, including newer segments not yet listed
   in the manifest;
6. require a contiguous LSN history and replay only `LSN > checkpoint_lsn`;
7. truncate only an incomplete record at the tail of the final segment, then
   fsync the file and directory;
8. swap the fully validated temporary store into the live store.

A checksum failure, middle-of-file corruption, LSN gap, missing segment,
invalid manifest, or resource-limit breach returns a nonzero startup exit. The
process does not listen, does not publish a partial store, and does not modify
the damaged image (except the one permitted tail truncation).

The exact byte layouts are in [wal-format.md](wal-format.md) and
[rdb-format.md](rdb-format.md).

## Startup and shutdown

Startup order is:

1. parse command-line options;
2. block SIGINT/SIGTERM through signalfd;
3. bind the listening socket;
4. acquire the writer lock and run recovery;
5. begin accepting connections.

A missing persistence image starts an empty database and is bootstrapped on the
first write or checkpoint. Any other recovery failure aborts startup with a
nonzero exit code and does not overwrite the damaged image.

On SIGINT/SIGTERM, the event loop stops, the TTL scanner is joined, and the
server performs a final checkpoint. Exit code 0 requires `RdbSaveResult::Ok`.
A `DurabilityUnconfirmed` result returns nonzero and warns that the snapshot may
already be visible.

## Failure and resource boundaries

- Protocol errors are converted to bounded error responses where the framing is
  still trustworthy.
- Truncated frames wait for more input until the configured frame/pending limit;
  over-limit input fails closed.
- Socket write failure tears down the connection without stopping the server.
- Peer reset and EPIPE are ordinary connection failures.
- Connection close callbacks are idempotent.
- The listener remains available after individual client failures.
- Persistence I/O failure after an append or fsync failure is fail-stop for the
  process; recovery decides the last valid prefix on restart.
- A second writer is rejected by the advisory lock with a nonzero exit.

The exact default bounds are:

| Resource | Limit |
|---|---:|
| Concurrent connections per process | 32 |
| Per-connection input/output budget | 1.25 MiB each |
| Key | 1 KiB |
| Value / WAL record payload | 1 MiB |
| WAL segment | 16 MiB |
| Total WAL bytes | 256 MiB |
| WAL segments | 128 |
| Recovery scanned records | 1,000,000 |
| Recovery scanned bytes | 512 MiB |
| RDB snapshot | 64 MiB / 100,000 records |

## Testing strategy

The local evidence suite covers:

- base RAII, Buffer, clocks, logging, and error utilities;
- EventLoop timers and callback-exception containment;
- HTTP parser framing, exact status mappings, and incremental 64 KiB+ rejection;
- HTTP real-socket routes, fragmented input, keep-alive, pipelining, slow reads,
  peer reset, `/stats`, and checkpoint behavior;
- KV framing, fragmentation, large binary payloads, TTL semantics, `STATS`, and
  malformed frame closure;
- WAL record and manifest codecs, CRC32C, LSN continuity, rotation, cleanup, and
  resource limits;
- checkpoint publication ordering, snapshot/manifest durability failures, and
  legacy RDB import;
- C01-C14 deterministic crash points, fixed-seed random `SIGKILL`, torn WAL
  tails, middle corruption, missing segments, and TTL recovery;
- process-level CLI, default path, restart persistence, corrupt startup,
  writer-lock conflict, and shutdown-save failure behavior;
- sanitizer, clang-format, clang-tidy, four fixed-seed fuzz targets, and the
  reliability benchmark;
- group-commit writer ordering, batch limits, queue/backlog limits, append and
  fsync failure, cleanup barriers, fail-stop, and exactly-once completion;
- HTTP and KV asynchronous mutation flows, client-disconnect safety, pipelined
  response ordering, SAVE/checkpoint barriers, and shutdown failure exits;
- R4-A timing counters, fixed fsync buckets, the top-level `group_commit`
  contract, and protocol checks in both HTTP and KV paths;
- the external G3 batch-formation experiment and its frozen gate checks.

The acceptance matrix runs Debug, Release, ASan, UBSan, ASan+UBSan, and TSan
from separate build directories. Passing it is local reproducibility evidence,
not a substitute for a public CI run or a production qualification.

## Deliberate non-goals

- TLS, authentication, authorization, or multi-tenant isolation.
- Replication, sharding, clustering, consensus, or transactions.
- HTTP/2, WebSocket, chunked requests, or Expect/100-continue.
- Multi-process sharing of one data directory.
- A public `BATCHED` durability mode. Group commit is an internal `SYNC`
  optimization; its implementation contract is in
  [ADR-006](decisions/006-group-commit-implementation.md).
- Background/incremental checkpoint, transactions, MVCC, or multiple writers.
- Power-loss, kernel-crash, firmware-cache, or media-failure proofs.
- Production SLAs, capacity claims, or arbitrary client scaling.

These exclusions are part of the contract, not hidden implementation gaps.

## Related decisions

- [ADR-005: R4-A group-commit candidate design and evidence gates](decisions/005-group-commit.md)
- [ADR-006: R4-A group-commit implementation](decisions/006-group-commit-implementation.md)
