# MiniDB architecture

## Scope

MiniDB is a **crash-consistent single-node key/value store for bounded-resource
environments**. It owns its event loop, non-blocking TCP layer, HTTP and binary
protocol parsers, TTL store, WAL, RDB checkpoint format, checkpoint manifest,
and startup recovery path. The binary protocol also contains a durable Agent
state backend built on the same committed KV, WAL, and checkpoint machinery.

The project is a reliability engineering prototype, not a production database.
It does not claim SQL or Redis compatibility, high concurrency, replication,
serializable transactions, distributed transactions, or a production SLA. The
binary KV protocol provides bounded snapshot-isolation transactions with
first-committer-wins conflict detection; HTTP mutations remain independent
implicit transactions.

Two independent server processes expose the same storage engine:

- **minidb_http_server** exposes health, echo, save, stats, and key/value routes
  over a deliberately strict HTTP/1.1 subset.
- **minidb_kv_server** exposes CRUD, TTL, save, stats, explicit transactions,
  and `AGENT=12` durable Agent-state commands over a binary-safe framed TCP
  protocol. HTTP intentionally exposes no Agent route.

Both processes link the same network and storage libraries. They do not share
memory and must use distinct persistence base paths. A second process is
rejected by an advisory `flock` on `<rdb_path>.lock`.

## Design invariants

1. One owner EventLoop is the only mutation admission, in-memory apply,
   completion, and protocol-response point.
2. A dedicated writer thread exclusively owns WAL append, fsync, rotation, and
   cleanup for bounded group commit.
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
    snapshot, transaction, version, admission-queue, batch, and completion
    limits.
11. Explicit transactions use a BEGIN snapshot LSN. Reads first consult the
    private write set, then the newest committed version at or below that LSN;
    no dirty read is possible.
12. The transaction table is owner-loop state. At COMMIT admission, a write to a
    key whose committed or pending LSN is greater than the snapshot conflicts;
    otherwise one `kTxn` record is committed under one LSN and applied
    atomically.
13. Active transaction private writes never enter the live store, checkpoint,
    or manifest. Connection loss, session destruction, and shutdown roll back
    uncommitted transactions.
14. Version GC preserves every version required by an active snapshot and the
    latest committed version; it fails closed at the version budget rather than
    violating snapshot visibility.
15. Agent records live only under the reserved `\x00minidb-agent-v1\x00`
    namespace. Ordinary KV and HTTP entry points reject that prefix before any
    read or write, so internal state cannot leak through the public KV surface.
16. An Agent mutation and its idempotency result are applied by one internal
    `kTxn` at one LSN. A replay returns the stored `MDOR` without allocating an
    LSN or bumping a revision.
17. Agent recovery validates the namespace marker and every `MDAR` before the
    process listens. Unknown, malformed, conflicting, or orphaned Agent state
    is fail-stop rather than silently skipped.

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
mutations, manages the transaction table, submits admission entries, and is the
only thread that applies store mutations and invokes protocol completions. The
writer consumes a contiguous LSN prefix, encodes a physical batch, appends it to
the WAL, and performs one data fsync for the batch tail. It never touches
`KvStore`, the transaction table, sessions, sockets, or user callbacks.

Explicit transaction lifecycle, snapshot reads, private write-set updates,
conflict admission, and rollback also run on the owner loop. A `KvSession`
stores only its transaction id; it does not hold a pointer into a version chain.
COMMIT admission clears the session's transaction id immediately, so a later
disconnect cannot revoke an accepted commit. The actual atomic apply occurs when
the group-commit completion reaches the owner loop.

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

SAVE, checkpoint, interval flush, and shutdown issue writer barriers. A
background checkpoint has two short admission cuts. The first drains and applies
accepted records before WAL rotation and independent snapshot capture; the
second drains writes admitted while the worker writes the RDB before manifest
publication and cleanup. A manifest therefore never includes an LSN that has not
been applied. Shutdown failure is propagated to a nonzero process exit code.

## Component map

| Layer | Main files | Responsibility |
|---|---|---|
| Base | src/base | RAII descriptors, Buffer, clocks, error values, logging |
| Network | src/net | eventfd/timerfd/signalfd EventLoop, level-triggered epoll, Acceptor, TcpConnection, TcpServer |
| Protocol | src/protocol | HTTP parser/session/routes, binary KV framing/session, `AGENT=12` argument/response handling |
| Storage | src/storage | MVCC KvStore, transaction table/conflict admission, Agent record/state store and GC, RDB checkpoint, WAL, manifest, recovery, single-writer lock |
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
two-phase background checkpoint and reports failure or unconfirmed durability
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
5. `BEGIN`, `COMMIT`, and `ROLLBACK` use the owner-loop transaction table.
   Inside a transaction, SET/GET/DEL/EXPIRE/TTL read or stage private writes;
   COMMIT performs conflict admission before submitting one `kTxn` record.
   `AGENT` is rejected inside an explicit transaction without changing that
   transaction.
6. The response is encoded in the same frame layout and queued through
   TcpConnection.
7. The connection remains usable for command-level errors; framing corruption
   or an unknown command returns ERROR where possible and closes the connection.
   An uncommitted transaction is rolled back on close.

`STATS` (command code 8) returns the same persistence statistics JSON as the
HTTP endpoint, including the top-level `transactions` object. `SAVE` (command
code 7) runs the same two-phase background checkpoint state machine as HTTP POST
/save. HTTP has no BEGIN/COMMIT/ROLLBACK handle: each HTTP KV mutation is an
implicit one-operation transaction.

One command is in flight per connection. Pipelined frames remain buffered and
are resumed from write completion, not recursive callbacks.

## Agent state backend

The Agent backend is a typed state layer over committed KV records, not a second
storage engine:

- the reserved namespace holds a marker plus versioned `MDAR` records and
  idempotency entries;
- the owner-loop `AgentStateStore` indexes session lifecycle, parent/child tool
  state, lease deadlines, last fences, generic records, and idempotency hashes;
  indexes are rebuilt from committed records at startup;
- each mutation is converted into a bounded internal `kTxn` containing the
  state changes and idempotency record. The normal group-commit writer assigns
  the LSN and the owner loop applies both changes atomically;
- deterministic business rejections return an `MDOR` in a transport `VALUE`
  frame and do not write WAL. Protocol/parameter/resource/I/O failures use
  `ERROR`; a persistence admission race can use `CONFLICT=105` and should be
  retried with the same idempotency key;
- leases use monotonic live deadlines and durable epoch-millisecond expiry,
  strict revision/CAS checks, and a monotonically increasing fence. Recovery
  does not auto-release a lease or rewrite a RUNNING tool;
- a roughly one-second owner-loop timer runs bounded Agent GC. Each candidate
  group is deleted by an internal `kTxn`; a terminal/expired session and all of
  its tools are removed together.

Agent records are included in ordinary RDB snapshots, WAL replay, manifest LSN
boundaries, checkpoint publication, and MVCC version GC. There is no separate
Agent file, format version, or HTTP API. The stable `agent` object in `/stats`
and KV `STATS` reports counts, logical bytes, replay/conflict/fence counters, and
GC timing. Full wire semantics are in [protocol-kv.md](protocol-kv.md) and
[ADR-009](decisions/009-agent-state-backend.md).

## Storage and TTL semantics

KvStore stores keyed version chains under one mutex. Each committed mutation
appends a version tagged with its commit LSN; tombstones represent deletes.
Snapshot reads select the newest version with `commit_lsn <= snapshot_lsn`, and
the transaction's private write set takes priority for read-your-writes. The
standalone CRUD surface synthesizes a single committed version so existing
in-memory users still observe one current value. TTL deadlines use
`std::chrono::steady_clock` while the process is live. The WAL and RDB boundary
converts those deadlines to absolute Unix epoch milliseconds, so restarting the
process cannot extend a TTL.

- SET replaces a value and clears any TTL.
- A non-positive EXPIRE removes the key.
- TTL reports -1 for an existing key with no expiry, -2 for a missing or expired
  key, and a non-negative remaining duration otherwise.
- Expired entries are removed lazily and by the optional periodic scanner.
- An EXPIRE mutation is logged as an absolute deadline before it is applied.
- An expired version is reported missing; snapshot reads do not fall back to an
  older live version after the selected version's deadline passes.
- Version GC retains the latest committed version and all versions still needed
  by active snapshots, subject to the global 1,000,000-entry budget.
- A transaction-visible SET/DEL/EXPIRE is folded into one final operation per
  key before COMMIT emits the `kTxn` record.

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

`PersistenceManager::set`, `del`, and `expire` validate a standalone operation
and submit it as an implicit one-operation transaction to the bounded
group-commit writer with the next contiguous LSN. Explicit `BEGIN`/COMMIT uses
the same writer, but emits one `kTxn` record for the folded private write set.

At COMMIT admission, the owner checks
`max(committed_key_lsn, pending_key_lsn) > snapshot_lsn` for every written key.
A conflict returns `CONFLICT` without WAL or an LSN. A successful commit gets
exactly one LSN; read-only commits get LSN `0`, no WAL, and no LSN allocation.

- `SYNC`: the writer appends a physical batch and fsyncs its tail. The owner
  loop receives the completion, applies the mutations in LSN order, and only
  then reports success.
- `INTERVAL`: the owner loop applies after the append completes and reports
  success. The owner-loop timer, checkpoint, or shutdown later flushes and
  fsyncs the WAL.
- If the WAL append or required fsync fails, the persistence layer enters
  fail-stop: later mutations return an error until restart. LSN, byte counters,
  and statistics are not rolled back.
- A complete record that reached the page cache before an fsync failure may
  reappear after recovery. Unacknowledged writes are therefore allowed; an
  acknowledged `SYNC` write is not allowed to disappear.

### Checkpoint ordering

A checkpoint is a two-phase background state machine:

1. pause mutation admission, pause the group-commit writer, and drain every
   accepted record through owner-loop apply;
2. flush and fsync the WAL, rotate the active segment if it contains records,
   and capture an independent snapshot while scanner sweeps are excluded;
3. resume the group-commit writer and mutation admission;
4. on the checkpoint worker, serialize the independent snapshot, write the RDB
   temporary file, fsync it, rename it, and fsync the parent directory;
5. pause admission and the writer again, then drain records admitted while the
   RDB was being written;
6. atomically write and fsync a manifest naming `checkpoint_lsn` and the
   complete live WAL-segment set beginning at `checkpoint_lsn + 1`;
7. remove obsolete WAL segments and fsync the WAL directory;
8. resume admission, or finish shutdown, and complete the SAVE request.

Snapshot capture is O(records) and still occurs while admission is paused. RDB
serialization and durable file I/O do not hold admission or the live store.

A snapshot-durability or manifest-durability failure marks persistence
fail-stop. Old WAL is retained in every ambiguous case. Cleanup failure does not
invalidate a checkpoint: obsolete segments become residue, recovery ignores
segments before the first retained start, and the writer remains paused so a
later SAVE retries cleanup. A crash after RDB rename but before manifest publish
leaves the old manifest and complete old WAL as the recovery source.

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
| Agent sessions / tools | 1,024 / 8,192 |
| Tools per Agent session | 128 |
| Agent generic / idempotency records | 16,384 / 8,192 |
| Agent record / logical bytes | 64 KiB / 64 MiB |
| Agent names / tool names | 128 B / 256 B |

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
- sanitizer, clang-format, clang-tidy, six fixed-seed fuzz targets, and the
  reliability benchmark;
- group-commit writer ordering, batch limits, queue/backlog limits, append and
  fsync failure, cleanup barriers, fail-stop, and exactly-once completion;
- HTTP and KV asynchronous mutation flows, client-disconnect safety, pipelined
  response ordering, SAVE/checkpoint barriers, and shutdown failure exits;
- R4-A timing counters, fixed fsync buckets, the top-level `group_commit`
  contract, and protocol checks in both HTTP and KV paths;
- the external G3 batch-formation experiment and its frozen gate checks;
- snapshot isolation, read-your-writes, repeatable reads, no dirty reads,
  first-committer-wins conflicts, pending-commit conflict windows, read-only
  commits, transaction protocol framing, disconnect rollback, atomic `kTxn`
  recovery, TTL/snapshot selection, version GC, resource limits, and the
  top-level `transactions` stats contract;
- Agent record/MDOR codecs, strict revision/CAS, idempotency replay and
  conflicts, reserved-namespace isolation, lease expiry/fencing, tool state
  transitions, namespace recovery fail-stop, bounded GC, checkpoint/WAL replay,
  process-level crash recovery, and the top-level `agent` stats contract.

The acceptance matrix runs Debug, Release, ASan, UBSan, ASan+UBSan, and TSan
from separate build directories. Passing it is local reproducibility evidence,
not a substitute for a public CI run or a production qualification.

## Deliberate non-goals

- TLS, authentication, authorization, or multi-tenant isolation.
- Replication, sharding, clustering, or consensus.
- HTTP/2, WebSocket, chunked requests, or Expect/100-continue.
- Multi-process sharing of one data directory.
- A public `BATCHED` durability mode. Group commit is an internal `SYNC`
  optimization; its implementation contract is in
  [ADR-006](decisions/006-group-commit-implementation.md).
- Serializable isolation, predicate/range conflict detection, or prevention of
  snapshot-isolation write skew.
- HTTP cross-request transactions, distributed transactions, or multiple
  writers.
- Background/incremental checkpoint or automatic periodic checkpoint.
- Power-loss, kernel-crash, firmware-cache, or media-failure proofs.
- Production SLAs, capacity claims, or arbitrary client scaling.
- HTTP Agent routes, multi-node leases, consensus, cross-node session migration,
  or exactly-once external tool side effects.
- Generic Agent record deletion, TTL, or enumeration.

These exclusions are part of the contract, not hidden implementation gaps.

## Related decisions

- [ADR-005: R4-A group-commit candidate design and evidence gates](decisions/005-group-commit.md)
- [ADR-006: R4-A group-commit implementation](decisions/006-group-commit-implementation.md)
- [ADR-007: R4-B background full checkpoint](decisions/007-background-checkpoint.md)
- [ADR-008: R4-C MVCC and snapshot-isolation transactions](decisions/008-mvcc-transactions.md)
- [ADR-009: R4-D durable Agent state backend](decisions/009-agent-state-backend.md)
