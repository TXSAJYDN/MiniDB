# Recovery model

## Scope

Startup recovery reconstructs the live `KvStore` from three durable inputs:

~~~text
<rdb_path>              RDB v2 checkpoint snapshot
<rdb_path>.manifest     checkpoint manifest v1
<rdb_path>.wal/         WAL segments
~~~

The `<rdb_path>.lock` file is not data; it is the advisory single-writer lock.
Recovery runs before the listening socket accepts clients and publishes the
store only after the complete image validates.

## Preconditions

1. The process has parsed a valid command line.
2. SIGINT and SIGTERM are blocked through signalfd.
3. The listening socket is bound but not accepting.
4. The process holds `flock(LOCK_EX | LOCK_NB)` on `<rdb_path>.lock`.

A lock conflict is a startup failure with a nonzero exit code. The lock is
advisory: it coordinates cooperating MiniDB processes but cannot prevent an
unrelated program from editing the files.

## Manifest-missing cases

| RDB | WAL segments | Result |
|---|---|---|
| absent | none | Start empty; first mutation or checkpoint bootstraps RDB/manifest/WAL |
| present | none | Import the legacy RDB; first mutation or checkpoint bootstraps the WAL |
| present | present | Refuse: ambiguous migration without a manifest |
| absent | present | Refuse: WAL without an RDB snapshot or manifest |

The legacy import path does not write during recovery. The existing RDB is loaded
into a temporary store, and the first persistence write establishes the initial
manifest and WAL segment.

## Two-cut background-checkpoint boundary

At runtime, a checkpoint cannot snapshot an LSN that has been written or fsynced
by the writer but not yet applied by the owner loop. R4-B uses two short
admission cuts around background work:

1. pause new mutation admission and issue a writer pause barrier;
2. let the writer drain every record accepted before the barrier, return
   completions to the owner loop, and apply them in LSN order;
3. pause the TTL scanner, flush the WAL, and rotate the active segment when
   required;
4. capture an independent `RdbSnapshot` and record the applied LSN as
   `checkpoint_lsn`; then resume the writer and admission;
5. on the checkpoint worker, serialize the snapshot, write and fsync a temporary
   RDB, atomically rename it, and directory-fsync its parent;
6. pause admission again and issue a second writer barrier; drain and apply all
   mutations admitted while the worker was running;
7. collect the complete live WAL segment set beginning at `checkpoint_lsn + 1`,
   publish and directory-fsync the manifest, remove obsolete WAL prefix files,
   and resume the writer or finish shutdown.

A barrier failure keeps the writer paused or failed and prevents manifest
publication. Shutdown failure is reflected in a nonzero process exit code; a
failed graceful shutdown is not reported as success.

The writer remains the sole owner of WAL append, fsync, rotation, and cleanup.
The worker never reads the live `KvStore` and does not hold the store scanner
pause. Capture is still O(records) and runs inside the first admission cut; this
is not a zero-pause checkpoint.

## Manifest and live-segment boundary

The manifest records `checkpoint_lsn` and every retained segment from
`checkpoint_lsn + 1` through the current live chain. It is not valid to record
only the final active segment: writes admitted while the worker serializes or
writes the RDB may cause rotations, and all of those segments are required for
recovery.

If the process crashes after the new RDB rename but before manifest publication,
the old manifest remains authoritative. The new RDB may already contain state
ahead of the old `checkpoint_lsn`, but the complete old WAL still covers every
mutation after that checkpoint. Replay is deterministic because SET, DEL, and
EXPIRE are logical operations with stable LSN order and absolute expiry
deadlines.

Cleanup failure after a durable manifest is safe residue, not a checkpoint
failure. The manifest still defines the live chain, the writer remains paused
when cleanup needs retry, and the next SAVE captures a new boundary and retries
prefix cleanup.

## Normal recovery algorithm

1. Read and validate the manifest.
2. Require the manifest's RDB snapshot to exist and be a regular file.
3. Load the RDB into a fresh temporary `KvStore`. Verify magic, version, size,
   record count, varints, checksums, duplicate keys, expiry values, and recovery
   byte limits.
4. Resolve the live WAL chain:
   - the first retained segment start must equal `checkpoint_lsn + 1`;
   - every segment listed by the manifest must exist;
   - segments after the first retained start also participate, including ones
     rotated after the manifest was published.
5. Read each selected segment and decode records in order.
6. Require the first record of each segment to match the segment's start LSN.
7. Require the global LSN sequence to be contiguous.
8. Replay only records with `LSN > checkpoint_lsn`.
9. Validate recovery record and byte limits.
10. Publish the temporary store with `KvStore::swap` only after all steps pass.

## Tail truncation

Only the final selected segment may end with an incomplete record. The recovery
path truncates the file to the end of the last complete record, fsyncs the file,
and fsyncs the WAL directory. The number of removed bytes is reported as
`truncated_bytes`.

The following are not tail truncation and are fatal:

- a partial record in a non-final segment;
- a complete record with a bad CRC;
- a record with an invalid header, operation, key, value, expiry, or reserved
  field;
- trailing bytes after the last complete record;
- an LSN gap, duplicate, or segment-start mismatch;
- a manifest-referenced segment that is missing;
- a resource-limit breach.

The recovery path never scans forward for a plausible record boundary after a
corruption. It fails closed.

## Why old manifests can be valid

Snapshot publication and manifest publication are separate atomic operations.
A crash can leave:

- a new snapshot with the old manifest; or
- a new snapshot with the new manifest; or
- an old snapshot with the old manifest.

A new manifest with a missing or non-durable snapshot is not allowed by the
publication order. When an old manifest is observed with a new snapshot, the
checkpoint LSN in the old manifest is lower than the snapshot's actual content.
The old manifest still names the complete old live segment set; recovery replays
every WAL record after the old checkpoint LSN. SET, DEL, and EXPIRE are
idempotent at the logical level, so replay over the newer snapshot produces the
same final state without double-applying a semantic effect.

## Resource bounds

| Bound | Default |
|---|---:|
| Concurrent connections | 32 |
| Key size | 1 KiB |
| Value / WAL payload | 1 MiB |
| WAL segment | 16 MiB |
| Total WAL bytes | 256 MiB |
| Segments | 128 |
| Recovery records | 1,000,000 |
| Recovery bytes | 512 MiB |
| RDB snapshot | 64 MiB / 100,000 records |

Exceeding a recovery bound returns a resource-limit failure. The process exits
nonzero, does not listen, and does not publish a partial store.

## Failure behavior

| Failure | Startup result |
|---|---|
| Writer lock busy | nonzero exit, no listening |
| Manifest missing, no sidecars, no RDB | empty start and bootstrap |
| Legacy RDB, no sidecars | import and bootstrap |
| WAL without manifest | conflict, nonzero exit |
| Manifest without RDB | corrupt, nonzero exit |
| RDB corrupt | nonzero exit |
| WAL middle corruption | nonzero exit |
| WAL tail partial record | truncate + fsync, then start |
| Missing referenced segment | nonzero exit |
| LSN gap or duplicate | nonzero exit |
| Recovery limit exceeded | nonzero exit |
| I/O error | nonzero exit |

On a fatal recovery error the damaged files are preserved. The only write made
by recovery is the permitted tail truncation. This lets an operator diagnose the
image instead of losing evidence to an automatic reinitialization.

## Observable statistics

The recovery portion of `/stats` and `STATS` reports:

- `rdb_records`;
- `checkpoint_lsn`, `last_lsn`, `next_lsn`;
- `wal_records_scanned`, `wal_records_replayed`;
- `wal_bytes`, `truncated_bytes`, `wal_segments`;
- `ignored_residue_segments`;
- `legacy_import`, `needs_bootstrap`, `manifest_present`.

These fields come from one `PersistenceSnapshot`. Writer queue, completion
backlog, WAL bytes, and writer state share a coherent snapshot. They describe the
recovery that produced the currently running process, not a live re-scan of the
data directory.

## Related documents

- [WAL and manifest format](wal-format.md)
- [Crash consistency](crash-consistency.md)
- [Reliability evidence](reliability-evidence.md)
- [ADR-002](decisions/002-wal-and-crash-consistency.md)
- [ADR-004](decisions/004-wal-resource-policy.md)
- [ADR-006](decisions/006-group-commit-implementation.md)
