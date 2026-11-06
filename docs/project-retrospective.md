# Project retrospective

## What was built

MiniDB started as a staged systems-programming exercise and now has an end-to-end
local implementation of:

- a Linux level-triggered epoll reactor with eventfd wakeups, timerfd timers, and
  signalfd shutdown handling;
- non-blocking accept, read, write, short-write queuing, connection limits,
  backpressure, and bounded input/output buffers;
- a strict incremental HTTP/1.1 parser with explicit wire limits and precise
  status mappings;
- HTTP routes for health, echo, save, and binary-safe /kv/<key> CRUD;
- a binary-safe TCP KV protocol with CRUD, TTL, and explicit save commands;
- a mutex-protected in-memory store with monotonic TTL deadlines and lazy plus
  periodic expiry;
- RDB v2 snapshots with CRC32C, bounded decoding, temporary-store recovery, and
  atomic publication after complete validation;
- process-level tests for CLI validation, default RDB paths, persistence,
  corrupt-file startup, and shutdown save failures;
- CTest, sanitizers, libFuzzer targets, syscall tracing, and benchmark tooling.

The project deliberately remains a focused learning system rather than a
production database.

## What went well

The implementation kept the core mechanisms explicit. The parser, epoll loop,
frame decoder, TTL clock conversion, and RDB layout are all readable in the
repository instead of hidden behind a framework.

The test strategy improved substantially over the project's history:

- unit tests cover exact parser status codes, framing errors, varint boundaries,
  CRC32C, and store preservation;
- real-socket tests cover fragmented requests, keep-alive, pipelining, large
  payloads, slow readers, peer resets, and malformed frames;
- process tests validate the executable boundary, including defaults and exit
  codes;
- sanitizer builds exposed lifecycle errors that ordinary Debug/Release tests
  did not;
- fuzzing exercises both incremental parsers under ASan and UBSan.

A recurring lesson was that a test must prove the required execution path. For
example, a valid maximum varint followed by a truncated record is not a varint
overflow test, and an empty Store cannot prove preservation of an existing
database. The AI-018 regression work corrects exactly this class of false
confidence.

## What was difficult

### Lifecycle and ownership

The network layer has several overlapping lifetimes: Channel callback, connection
shared pointer, session map, server, storage, and event loop. Early shutdown and
reset tests exposed use-after-free paths. The durable fixes were to destroy the
network object before its dependent session map and to defer connection
destruction until the active event callback has returned.

### Backpressure and non-recursive scheduling

A send can complete synchronously. Calling the session parser directly from the
write-complete callback can recurse through pipelined requests and exhaust the
stack. The final design queues a drain on the owner EventLoop, stops reading
while a response is in flight, preserves the unconsumed input tail, and re-drives
bytes already held in user space.

### Framing strictness

HTTP and KV both require exact message boundaries. Ambiguous Content-Length,
chunked framing, trailing bytes, truncated records, and oversized frames are
rejected rather than guessed. This is intentionally stricter than a general web
server and is part of the test contract.

### Persistence failure semantics

A save is not just a write. It must account for short writes, fsync failures,
close failures, rename failures, parent-directory fsync, and concurrent callers.
A load must not publish a partially decoded store. Splitting these into explicit
RdbSaveResult values and a temporary-store recovery path made the behavior
testable.

### Evidence quality

Some earlier rounds reported aggregate green test counts without preserving the
right targeted evidence. A correct final report needs source provenance, exact
commands, exit codes, sanitizer flag checks, the targeted regression logs, fuzz
artifacts, and the final Git state. A local green test count is not a public CI
result and does not by itself satisfy a delivery stage.

## Remaining external gaps

The local implementation and tests can be strong while the A-H delivery stage is
still not fully accepted. Reasons come from the project's delivery contract:

- GitHub Actions may not have run on the actual repository; a checked-in workflow
  file is not a CI run.
- No public issue or pull request has been attempted or linked.
- The gh command line tool was not available in the authoritative environment.
- The implementation files are untracked, and this repair round is prohibited
  from committing or pushing automatically.
- The user has not explicitly authorized an external public contribution.
- The repository has no LICENSE file, so distribution and contribution terms are
  unresolved.

These are external or ownership decisions, not defects that a local C++ patch
can honestly mark as complete.

## Lessons to carry forward

1. Test the exact failure mechanism, not a nearby symptom.
2. Keep resource bounds in the parser and in the transport layer.
3. Never publish recovery state until the entire input is validated.
4. Treat synchronous write completion as a scheduling hazard.
5. Exercise real sockets for protocol behavior, dynamic ports, and bounded
   timeouts.
6. Run sanitizers and TSan from fresh build directories.
7. Preserve raw evidence: commands, exit codes, flags, logs, and artifacts.
8. Distinguish local verification, public CI, merged pull request, and stage
   acceptance.
9. Do not claim production readiness for a system with no TLS, auth, replication,
   WAL, or multi-writer coordination.

## Recommended next steps

For the owner of the repository:

1. review and commit the intended source, tests, and documentation;
2. add a license appropriate to the project's goals;
3. configure and actually run GitHub Actions on the repository;
4. open a public issue or pull request only with explicit authorization;
5. archive the final local acceptance evidence with the exact source digest;
6. decide whether v1 migration, authentication, or a WAL is a future goal.

The project is suitable for further systems study and controlled local testing.
It should not be described as production-ready.
