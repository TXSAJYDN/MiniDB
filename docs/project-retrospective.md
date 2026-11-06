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
- CTest, sanitizers, libFuzzer targets, syscall tracing, and timed multi-round
  benchmark tooling.

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
- fuzzing exercises both incremental parsers under ASan and UBSan;
- the Stage H timed benchmark preserves every raw latency sample and client/server
  resource sample instead of reporting only a favorable aggregate.

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
artifacts, raw latency/resource samples, and the final Git state. A local green
test count is not a public CI result and does not by itself satisfy a delivery
stage.

## Current delivery state

As of 2026-11-06, the P0 A-H technical acceptance is complete:

- the implementation is committed and pushed to `TXSAJYDN/MiniDB`;
- the product acceptance baseline is `9da4b48f5fe78e4c2896ad1ca07464361b27e362`;
- six fresh configurations each pass 214/214 CTest;
- GitHub Actions run `42182440543` completed successfully with 9/9 jobs;
- the default Stage H benchmark is a real 5-round, 10-second warm-up,
  30-second measurement, 5-second cooldown run per protocol, with 267,270
  successful HTTP requests and 295,147 successful KV requests, zero errors, and
  zero timeouts;
- the raw benchmark JSON is 11,780,123 bytes with SHA-256
  `8cfd8a89e6a73b70cc9abb6a4b180f01589bb1792b30c2a21f8c5393ba35afa8`;
- an external `hey` cross-check was completed at c1/c10/c32, with c32 at the
  server's 32-connection limit and the client CPU-bound; it is recorded as a
  cross-check, not a capacity claim;
- a real public-contribution attempt was made against
  `eclipse-4diac/4diac-forte#1135`; the patch and tests exist locally, but the
  pull request was not created because upstream/fork pushes and anonymous PR
  creation were denied;
- the owner explicitly deferred the license decision, so no LICENSE or COPYING
  file was created.

## Remaining external gaps

The A-H P0 technical gate is closed, but these constraints remain real:

- the owner has deliberately deferred the license; distribution terms are still
  unresolved;
- the 4diac contribution attempt did not produce a pull request because the
  environment lacked a writable fork and authenticated PR access, and Eclipse ECA
  completion is not claimed;
- hardware performance counters are unavailable under this WSL2 host, so only
  software counters are reported;
- the 5×30-second benchmark is a same-host closed-loop WSL2 baseline with a
  Python client. It does not establish open-loop capacity, saturation, scaling
  beyond 32 connections, or production SLA;
- the project has no TLS, authentication/authorization, replication, consensus,
  sharding, transactions, WAL, or multi-process RDB ownership.

These are delivery or ownership constraints, not permission to relabel a local
test result as a public contribution or a production guarantee.

## Lessons to carry forward

1. Test the exact failure mechanism, not a nearby symptom.
2. Keep resource bounds in the parser and in the transport layer.
3. Never publish recovery state until the entire input is validated.
4. Treat synchronous write completion as a scheduling hazard.
5. Exercise real sockets for protocol behavior, dynamic ports, and bounded
   timeouts.
6. Run sanitizers and TSan from fresh build directories.
7. Preserve raw evidence: commands, exit codes, flags, logs, samples, and hashes.
8. Distinguish local verification, public CI, merged pull request, and stage
   acceptance.
9. Do not claim production readiness for a system with no TLS, auth, replication,
   WAL, or multi-writer coordination.

## Recommended next steps

For the owner of the repository:

1. decide the license when ready and add the corresponding LICENSE/COPYING;
2. configure a writable fork and authenticated contribution path, complete
   Eclipse ECA if required, and retry the 4diac PR;
3. if productionization is a goal, plan TLS/auth, replication and consistency,
   multi-process ownership, WAL/transactions, long-running stress tests, and
   cross-platform validation;
4. keep the six-configuration, fuzz, failure-injection, and CI gates in place;
5. preserve the raw Stage H benchmark JSON and its hash rather than replacing it
   with only rounded summary numbers.

The project is suitable for further systems study and controlled local testing.
It should not be described as production-ready.
