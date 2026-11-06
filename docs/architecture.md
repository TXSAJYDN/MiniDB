# MiniDB architecture

## Scope

MiniDB is a single-node educational key/value database. It owns its event loop,
non-blocking TCP layer, HTTP and binary protocol parsers, TTL store, and RDB
snapshot format. It is not a general-purpose web server or a production
database.

The project has two independent server processes:

- **minidb_http_server** exposes health, echo, save, and key/value routes over a
  deliberately strict HTTP/1.1 subset.
- **minidb_kv_server** exposes CRUD, TTL, and save commands over a binary-safe
  framed TCP protocol.

Both processes use the same network and storage libraries. They do not share
memory and should not be pointed at the same RDB file at the same time.

## Process and thread model

Each server process creates one EventLoop on the main thread. Socket readiness,
timers, signal handling, protocol parsing, command execution, and snapshot
initiation all run on that owner thread. This removes data races inside the
connection state machine and makes callback ordering deterministic.

Each KvStore may run one background TTL scanner. The scanner holds the same
store mutex as commands, removes expired entries, and does not touch socket
callbacks. A second process cannot safely share the same RDB path because there
is no inter-process lock.

The design intentionally has no worker pool. A slow filesystem operation such
as SAVE blocks the owner loop, but the operation is synchronous and its
completion is reported before the client receives a success response.

## Component map

| Layer | Main files | Responsibility |
|---|---|---|
| Base | src/base | RAII descriptors, Buffer, clocks, error values, logging |
| Network | src/net | eventfd/timerfd/signalfd EventLoop, level-triggered epoll, Acceptor, TcpConnection, TcpServer |
| Protocol | src/protocol | HTTP parser/session/routes and binary KV framing/session |
| Storage | src/storage | thread-safe in-memory KvStore and RDB v2 save/recovery |
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
5. HttpServer routes the request to health, echo, save, or /kv/<key> logic.
6. HttpResponse serializes the status line, headers, and body.
7. The session stops reading while a response is in flight. Write completion
   resumes draining buffered pipelined requests or re-enables socket reads.

The parser uses explicit CRLF framing and Content-Length. It rejects ambiguous
or unsupported framing rather than guessing. Request-line, target, header,
field-count, body, and pending-buffer limits are documented in
[protocol-http.md](protocol-http.md).

### Keep-alive and pipelining

HTTP/1.1 connections are persistent by default. The server permits pipelined
requests but processes one at a time and preserves response order. A request
with Connection: close, a final parse error, or an oversized input causes the
session to stop reading and shut down after queued output is flushed.

### Idle timeout

An incomplete or idle HTTP request gets a 30-second owner-loop timer. New bytes
restart the incomplete-request interval. The timeout sends 408 and closes the
connection. The KV protocol has no idle timeout.

## Binary KV request path

1. KvServer creates one KvSession per connection.
2. KvSession appends bounded input, then schedules an owner-loop drain.
3. KvProtocol::parse_command() consumes exactly one frame at a time and returns
   incomplete, malformed, or complete without resynchronizing.
4. KvSession validates argument counts, key sizes, and TTL text, then calls
   KvStore.
5. The response is encoded in the same frame layout and queued through
   TcpConnection.
6. The connection remains usable for command-level errors; framing corruption
   or an unknown command returns ERROR where possible and closes the connection.

One command is in flight per connection. Pipelined frames remain buffered and
are resumed from write completion, not recursive callbacks.

## Storage and TTL semantics

KvStore stores values in std::unordered_map under one mutex. TTL deadlines use
std::chrono::steady_clock while the process is live. The RDB boundary converts
those deadlines to absolute Unix epoch milliseconds.

- SET replaces a value and clears any TTL.
- A non-positive EXPIRE removes the key.
- TTL reports -1 for an existing key with no expiry, -2 for a missing or expired
  key, and a non-negative remaining duration otherwise.
- Expired entries are removed lazily and by the optional periodic scanner.

The exact wire rules are in [protocol-kv.md](protocol-kv.md).

## Persistence path

RdbManager serializes a complete snapshot into memory, appends CRC32C, writes a
temporary file with O_EXCL, fsyncs it, closes it, renames it over the target, and
fsyncs the parent directory. Save operations are serialized with a mutex.

Recovery reads and validates the whole file into a temporary KvStore. It checks
size, checksum, magic, version, record count, every varint, every key/value
length, duplicate keys, expiry validity, and total recovery bytes. Only after the
entire image is valid does it swap the temporary store into the live store. A
corrupt file returns -1 and leaves the live store untouched.

The exact layout and limits are in [rdb-format.md](rdb-format.md).

## Startup and shutdown

Startup order is:

1. parse command-line options;
2. block SIGINT/SIGTERM through signalfd;
3. bind the listening socket;
4. load and validate the RDB;
5. begin accepting connections.

A missing RDB starts an empty database. Any other load failure aborts startup
with a nonzero exit code and does not overwrite the damaged file.

On SIGINT/SIGTERM, the event loop stops, the server performs a final save, and
the process exits 0 only if the snapshot write and parent-directory fsync are
confirmed. A visibility-unconfirmed directory fsync maps to a distinct failure
message and nonzero exit status.

## Failure and resource boundaries

- Protocol errors are converted to bounded error responses where the framing is
  still trustworthy.
- Truncated frames wait for more input until the configured frame/pending limit;
  over-limit input fails closed.
- Socket write failure tears down the connection without stopping the server.
- Peer reset and EPIPE are ordinary connection failures.
- Connection close callbacks are idempotent.
- The listener remains available after individual client failures.
- RDB save failure does not claim durability; a changed-but-unconfirmed rename
  is reported separately.

## Testing strategy

The local evidence suite covers:

- base RAII, Buffer, clocks, logging, and error utilities;
- EventLoop timers and callback-exception containment;
- HTTP parser framing, exact status mappings, and incremental 64 KiB+ rejection;
- HTTP real-socket routes, fragmented input, keep-alive, pipelining, slow reads,
  and peer reset;
- KV framing, fragmentation, large binary payloads, TTL semantics, and malformed
  frame closure;
- RDB format vectors, CRC32C, varint overflow, corruption matrices, save/load,
  TTL recovery, and non-empty-store preservation;
- process-level CLI, default path, restart persistence, corrupt startup, and
  shutdown-save failure behavior;
- sanitizer, fuzz-smoke, syscall-trace, and benchmark evidence when the
  corresponding tools are available.

The acceptance matrix is intentionally stricter than a single local build:
Debug, Release, ASan, UBSan, ASan+UBSan, and TSan are configured and tested from
fresh directories. Passing that matrix is local reproducibility evidence, not a
substitute for a public CI run.

## Deliberate non-goals

- TLS, authentication, authorization, or multi-tenant isolation.
- Replication, sharding, clustering, consensus, or transactions.
- HTTP/2, WebSocket, chunked requests, or Expect/100-continue.
- Multiple writers to one RDB path.
- Production SLAs, crash-consistent WAL recovery, or arbitrary client scaling.

These exclusions are part of the contract, not hidden implementation gaps.
