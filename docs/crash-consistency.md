# Crash consistency

## What is claimed

MiniDB claims **process-crash consistency** for the paths covered by its tests:

> If a client receives a successful response for a mutation in `SYNC` mode, a
> later process crash and restart must recover that mutation, assuming the
> tested file and device model holds.

The claim does not cover:

- power loss or sudden machine reset;
- kernel panic or filesystem failure;
- storage firmware that acknowledges an fsync without making data durable;
- media corruption or bit rot;
- arbitrary third-party modification of the files;
- more than one writer sharing a data directory.

These are separate failure models and require separate evidence.

## Ack set and possible set

A crash test cannot require the state after a crash to be byte-identical to the
state before the crash. The correct comparison is between two sets:

- **ack set**: mutations for which the client received a success response;
- **possible set**: mutations whose complete records may have reached durable
  storage before the crash, including unacknowledged writes.

Required properties:

1. Every operation in the ack set is present after recovery.
2. No operation outside the possible set is present.
3. No partially applied key/value is visible.
4. A deleted key is not resurrected by replaying an older record out of order.
5. TTL deadlines are not extended by restart.

In `SYNC` mode, a write enters the possible set only after append and fsync can
have completed. A complete record may reach the page cache before an fsync
failure; recovery may therefore observe an unacknowledged write. That is allowed.
An acknowledged write disappearing is not allowed.

For explicit transactions, BEGIN, ROLLBACK, conflict, and a read-only COMMIT do
not enter either set because they write no WAL. A complete COMMIT record may be
in the possible set even if the client did not receive its success response.
After recovery, that transaction must appear with every key update or not at all.
In `SYNC` mode, a successful COMMIT response means the complete transaction is
in the ack set and every key it wrote must survive a process crash.

`INTERVAL` deliberately widens the possible set: an acknowledged write is not
guaranteed durable until the next flush. The window is bounded by the 100 ms
timer, checkpoint, or graceful-shutdown flush, subject to scheduling and I/O.

## Crash points C01-C14

| Point | Scenario | Allowed result | Forbidden result |
|---|---|---|---|
| C01 | Crash before WAL append | Old state remains | Partial or invented mutation |
| C02 | Half of WAL record header | Tail truncated; old state | Partial record applied |
| C03 | Half of WAL payload | Tail truncated; old state | Partial key/value visible |
| C04 | Complete record in page cache before fsync | Record may reappear intact after process crash | Corrupt partial record |
| C05 | Fsync before memory apply | Record is replayed | Lost complete record |
| C06 | Crash after apply before ack | Applied value may reappear | Wrong or partial value |
| C07 | Crash during snapshot temp write | Old snapshot plus WAL recovers | WAL removed too early |
| C08 | Snapshot fsynced before rename | Old snapshot plus WAL recovers | New snapshot assumed durable without rename |
| C09 | Old manifest plus new snapshot and complete old WAL | Deterministic replay over the new snapshot | Missing or double-applied semantic effect |
| C10 | Half-written manifest | Old manifest remains authoritative | Torn manifest accepted |
| C11 | Manifest renamed before directory fsync | Old or new manifest remains recoverable | Snapshot/manifest mismatch that loses ack set |
| C12 | Crash during obsolete WAL unlink | Published manifest defines the live chain | Deleted segment required by the manifest |
| C13 | Fixed-seed random SIGKILL, 20,000 operations | Every acknowledged write survives | Any ack-set loss or partial value |
| C14 | Injected I/O errors | Error returned and persistence fails closed | False success or continued unsafe writes |

C04 is intentionally narrow: it demonstrates that a complete record may be
visible after a process crash and that recovery handles it, not that the record
survives power loss. C09 uses an old manifest and the complete old WAL on a newer
snapshot to exercise the publication ordering. C13 is deterministic because the
seed and operation schedule are fixed; a failure can be reproduced from the seed
and preserved files.

## Transaction crash semantics

R4-C adds these required properties:

- one COMMIT is one WAL record and one LSN; recovery applies every operation in
  that record atomically or rejects the record without publishing a prefix;
- a crash after BEGIN or during transaction-local writes leaves no committed
  state because the private write set is memory-only;
- ROLLBACK and conflict produce no WAL and cannot resurrect an older value after
  restart;
- checkpoint capture excludes active transaction-private write sets; a crash may
  discard those uncommitted writes while preserving every committed `kTxn`
  record through manifest/WAL replay;
- version GC must not remove a version required by an active snapshot or change
  the state produced by replay;
- a torn outer transaction record is truncatable only at the final WAL tail; a
  complete record with an invalid MTXN payload is fatal and never partially
  applied.

The process suite exercises transaction commit/rollback, disconnect rollback,
conflict handling, stats shape, and recovery after a committed transaction
record. Unit tests cover malformed MTXN payloads, duplicate keys, atomic replay,
snapshot visibility, TTL selection, and version-budget behavior.

## Checkpoint ordering under crash

R4-B uses two admission cuts and a background worker:

1. first cut: pause mutation admission, pause and drain the writer, apply every
   accepted record, pause the TTL scanner, flush the WAL, rotate the active
   segment when needed, and capture an independent snapshot;
2. resume admission and the writer; the checkpoint worker serializes the
   snapshot, writes and fsyncs a temporary RDB, renames it, and directory-fsyncs
   its parent;
3. second cut: pause admission and drain/apply writes admitted while the worker
   ran;
4. write, fsync, rename, and directory-fsync a manifest naming the complete live
   WAL segment set beginning at `checkpoint_lsn + 1`;
5. remove obsolete WAL segments and fsync the WAL directory, then resume the
   writer or finish shutdown.

The important crash boundaries are:

- **before snapshot rename**: the old snapshot remains authoritative; WAL still
  contains every mutation after the old checkpoint;
- **after snapshot rename, before manifest publish**: the new snapshot may be
  visible, but the old manifest and its complete WAL segment set are sufficient;
  replay from the old checkpoint LSN is deterministic;
- **after manifest publish, before WAL cleanup**: the new manifest names the
  complete live segment set; leftover old segments are residue and are ignored;
- **during WAL cleanup**: the manifest already defines the live chain; deleting a
  segment it references would be a bug, not an allowed outcome.

Snapshot and manifest writes use temporary files, file fsync, atomic rename, and
parent-directory fsync. A directory-fsync failure after rename is reported as
durability unconfirmed, marks persistence fail-stop, and leaves the WAL intact.
The dedicated process test `BackgroundCheckpointRenameCrash` exercises the
rename-before-manifest window. Cleanup failure after manifest publication is
safe residue and can be retried; it is not evidence that the published
checkpoint failed.

## Tail versus middle corruption

The WAL is an append-only sequence of complete records. A process crash can leave
the final write half-complete, but cannot legitimately punch a hole in the middle
of the history. Therefore:

- an incomplete final record is truncated to the last complete record and
  fsynced;
- an incomplete record in a non-final segment is fatal;
- a CRC failure, invalid field, LSN gap, or segment-start mismatch is fatal;
- recovery never searches for the next plausible header after a bad record.

This policy is what prevents silent data loss from being reported as a clean
startup.

## Evidence

The deterministic crash points are implemented in
`tests/CrashConsistencyTest.cpp`. Process-level cases cover:

- HTTP and KV `SYNC` crash recovery;
- `INTERVAL` flush recovery;
- tail truncation;
- manifest corruption;
- missing WAL segment;
- WAL middle corruption;
- random fixed-seed `SIGKILL`;
- TTL crash recovery;
- explicit transaction contract, disconnect rollback, and committed `kTxn`
  recovery.

Four fuzz targets exercise the HTTP parser, KV frame parser, WAL record codec,
and checkpoint manifest codec:

~~~text
minidb_http_parser_fuzz
minidb_kv_protocol_fuzz
minidb_wal_record_fuzz
minidb_checkpoint_manifest_fuzz
~~~

The current fixed seed is `20261111`, with 20,000 runs per target. A non-empty
artifact directory is a failure; any crashing input must be minimized, added to
the corpus, and covered by a durable regression test.

## Limitations

- Tests use real files under WSL2 ext4 and injected file-operation failures.
- The random kill case is fixed-seed, not an exhaustive proof over all
  instruction interleavings.
- No test controls storage-controller caches or power removal.
- `INTERVAL` acknowledges before fsync by design; its loss window is part of the
  contract.
- The single-writer lock is advisory and does not defend against non-cooperating
  processes.

These limitations are stated with the evidence in
[reliability-evidence.md](reliability-evidence.md).
