# Reliability evidence

## Scope

This document records the R0-R3 baseline, the R4-A instrumentation and G3
batch-formation experiments, the implemented group-commit validation, the R4-B
background full-checkpoint validation, the R4-C snapshot-isolation MVCC
transaction validation, and the R4-D durable Agent state-backend validation for
MiniDB's crash-consistent persistence work. It is deliberately a statement
about a tested engineering prototype, not a production qualification.

The evidence covers:

- WAL, manifest, checkpoint, recovery, and single-writer lock correctness;
- deterministic crash points and process-level crash recovery;
- sanitizer, fuzz, format, and static-analysis gates;
- durability-mode performance, checkpoint pause, recovery time, and RSS;
- R4-A diagnostic timing fields, fixed WAL data-fsync buckets, and the
  top-level group-commit metrics contract;
- an external batch-formation opportunity experiment, the implemented bounded
  group-commit writer, and its clean-HEAD G4 benefit run;
- the implemented two-phase background checkpoint, its phase timings under a
  concurrent-writer probe, and its clean-HEAD formal artifact;
- per-key MVCC version chains, snapshot isolation, first-committer-wins,
  single-record transaction WAL commits, bounded GC/TTL behavior, and their
  protocol/recovery evidence;
- durable Agent sessions, tools, leases/fences, generic records, idempotency,
  bounded GC, namespace isolation, and their protocol/recovery evidence;
- explicit limits and non-claims.

## Source and environment

- Date: 2026-11-12 (Asia/Shanghai)
- Source worktree: `/home/win11/projects/miniDB`
- Baseline commit at the start of the upgrade: `34dcc79`
- Environment: Ubuntu 24.04 WSL2, kernel `6.18.58.1-microsoft-standard-WSL2`,
  x86_64, GCC 13.3.0, CMake 3.28.3, Python 3.12.3
- CPU: Intel Core i9-14900HX, 32 logical CPUs visible to WSL2
- Filesystem: WSL2 ext4

The historical R3 benchmark JSON records `git_head=34dcc79` and
`working_tree_dirty_at_collection=true`, because the reliability implementation
was still uncommitted when that artifact was collected. The metadata is retained
honestly rather than rewritten. The baseline commit `cfbda60` was later pushed
to `main`, where its GitHub Actions run passed all 9 jobs: debug, release, ASan,
UBSan, ASan+UBSan, TSan, format, static analysis, and fuzz smoke. This statement
is about that baseline commit, not a claim about every later commit.

## Test matrix

The local matrix for R4-D runs each configuration from its own CMake build
directory:

| Configuration | CTest |
|---|---:|
| Debug | 440/440 |
| Release | 440/440 |
| ASan | 440/440 |
| UBSan | 440/440 |
| ASan + UBSan | 440/440 |
| TSan (`setarch x86_64 -R`) | 440/440 |

Each configuration includes 31 `ProcessIntegrationTest.*` cases. The R4-A
historical matrix was 338 tests and 24 process cases; R4-B was 348/25; R4-C was
395/26. Those historical counts are not reused as R4-D evidence.

~~~text
CliContract
CliDurabilityContract
HttpPersistence
KvPersistence
TtlScannerLifecycle
CorruptRdbStartup
SaveFailureExit
ShutdownFailureExit
HttpSyncCrashRecovery
KvSyncCrashRecovery
IntervalFlushRecovery
BackgroundCheckpointRenameCrash
WriterLockConflict
WalTailTruncation
ManifestCorruption
MissingWalSegment
WalMiddleCorruption
LegacyImport
RandomKillSeed
TtlCrashRecovery
StatsContract
TransactionContract
KvAsyncProtocol
KvDisconnectSafety
HttpAsyncProtocol
HttpDisconnectSafety
AgentIdempotencyCrashRecovery
AgentLeaseTakeover
AgentCrashRecovery
AgentNamespaceCollision
AgentStatsContract
~~~

The unit and integration inventory also includes:

- WAL record codec and CRC32C boundaries;
- WAL manager append, short write, EINTR, fsync, rotation, cleanup, and limits;
- checkpoint manifest round-trip, truncation, ordering, and atomic publication;
- recovery selection, manifest-missing cases, tail truncation, middle
  corruption, LSN continuity, legacy import, and TTL replay;
- persistence manager `SYNC`/`INTERVAL`, asynchronous admission/completion,
  checkpoint retry, fail-stop, writer snapshots, and single-writer lock;
- group-commit writer ordering, 256-record/16-MiB batch limits, queue and
  completion backlog entry/byte limits, exactly-once callbacks, append/fsync
  failure, cleanup barriers, and writer stop/drain;
- HTTP/KV asynchronous mutation and disconnect no-op behavior, pipelined
  response ordering, SAVE/checkpoint barriers, and shutdown failure exit codes;
- background-checkpoint state transitions, independent snapshot capture,
  complete live WAL-segment retention, cleanup retry, manager-destruction
  callback safety, and crash recovery between RDB rename and manifest publish;
- per-key MVCC visibility at a snapshot LSN, read-your-writes, TTL expiry without
  old-version resurrection, version GC, and global version/transaction/write-set
  limits;
- transaction-table lifecycle, first-committer-wins conflicts including the
  accepted-but-not-applied pending-commit window, resource errors, and exactly
  one callback after session destruction;
- WAL v2 transaction codec/replay, atomic recovery of committed `kTxn` records,
  v1/v2 coexistence, and no WAL allocation for read-only commits, rollbacks, or
  conflicts;
- BEGIN/COMMIT/ROLLBACK protocol validation, `CONFLICT=105` mapping, transaction
  stats schema, disconnect rollback, and process-level transaction persistence;
- Agent MDAR/MDOR codecs, canonical request hashing, strict revision/CAS,
  idempotency replay/conflict behavior, reserved-namespace isolation, session
  and tool state transitions, lease expiry/fencing, bounded GC, and recovery
  fail-stop for malformed or orphaned records;
- process-level Agent idempotency crash recovery, lease takeover, RUNNING-tool
  recovery, namespace collision, and the `agent` stats contract;
- C01-C14 deterministic crash points.

This is local reproducibility evidence. It is not a substitute for a public CI
run. The R0-R3 baseline commit `cfbda60` and the R4-A implementation commit
`a06d7c2` both passed 9/9 GitHub Actions jobs. The R4-A G4 evidence commit, the
R4-B evidence commit, each R4-C implementation/evidence commit, and the R4-D
implementation/evidence commit must pass their own full CI run; status is read
from the GitHub checks for that exact commit, not inherited from an earlier one.
The R4-D ADR records the final implementation/evidence commit and run after the
remote matrix completes.

The first R4-D evidence run, commit `10ca3f1` in workflow run
[`43099105106`](https://github.com/TXSAJYDN/MiniDB/actions/runs/43099105106),
completed 8/9: the `asan-ubsan` job exited 8 while the other eight jobs passed.
The failed job log is not accessible without an authenticated Actions session,
so the exact test name was not recovered from the service. The identical local
ASan+UBSan configuration then passed 440/440, and
`ctest --repeat until-fail:5 -j2` completed five consecutive 440/440 passes
(2,200 test executions). The docs-only follow-up commit `c07b977` then passed its own full workflow
[`43101139773`](https://github.com/TXSAJYDN/MiniDB/actions/runs/43101139773)
with 9/9 jobs; this earlier failure remains recorded rather than being rewritten.

## Fuzz evidence

Six Clang libFuzzer targets are built with ASan and UBSan:

| Target | Corpus | Runs | Seed |
|---|---|---:|---:|
| `minidb_http_parser_fuzz` | `fuzz/corpus/http` | 20,000 | 20261111 |
| `minidb_kv_protocol_fuzz` | `fuzz/corpus/kv` | 20,000 | 20261111 |
| `minidb_wal_record_fuzz` | `fuzz/corpus/wal` | 20,000 | 20261111 |
| `minidb_checkpoint_manifest_fuzz` | `fuzz/corpus/manifest` | 20,000 | 20261111 |
| `minidb_agent_record_codec_fuzz` | `fuzz/corpus/agent_record` | 20,000 | 20261111 |
| `minidb_agent_protocol_fuzz` | `fuzz/corpus/agent_protocol` | 20,000 | 20261111 |

A clean run requires exit code 0 for every target, no ASan/UBSan diagnostic, and
an empty artifact directory. Any crash must be minimized, added to the corpus,
and covered by a durable regression test before the run is accepted again.

## R4-A instrumentation baseline

R4-A was evidence-gated. The v2 baseline below was collected before group
commit existed; it remains the frozen comparison point. The implemented server
now routes mutations through the bounded `GroupCommitWriter` and preserves the
documented durability modes.

The v2 benchmark records successful-operation timings from
`std::chrono::steady_clock`:

- `wal_append_*`: `write_all()` only; excludes encoding, rotation, and fsync;
- `wal_data_fsync_*`: successful WAL data fsyncs, including empty-segment
  creation, but excluding directory fsyncs;
- `sync_mutation_*`: successful `SYNC` from post-validation persistence
  admission through in-memory apply; includes bootstrap and checkpoint retry
  work, excludes network parsing and response serialization;
- `fsync_latency_bucket_counts`: fixed 50 us through 100 ms upper bounds plus
  an overflow bucket; counts must sum to `wal.data_fsyncs`.

`total` fields support measured-window means. `max` fields are process-lifetime
maxima that include preload/warm-up. The fixed buckets do not provide arbitrary
quantiles or per-request traces. ADR-005 defines G1-G7: the baseline and
separate batch-formation experiment had to satisfy G1-G3 before implementation,
and G4 later validated the implemented path on a clean HEAD.

### v2 baseline artifact

- `bench/results/reliability-20261111-r4a-baseline.json`
- 310,067 bytes
- SHA-256:
  `55bcad4ead05fcba26e44cecf17e71df6ffd7dba17e54a128398c5b65820fe84`
- `git_head=964ba8446a71f6c491cd1241e5479632465b9835`
- `working_tree_dirty_at_collection=false`
- 18 configurations, 5 rounds per configuration, 2 seconds per measured round
- 414,688 successful requests, 0 errors
- 229.087 seconds wall-clock collection time
- dataset: 128 keys, 64-byte values; seed `20261111`
- same WSL2 host, closed-loop persistent KV connections
- recovery: `SIGKILL` after each measured round, then fresh-process startup to
  `PING`; checkpoint: KV `SAVE` after recovery

The v2 artifact does not overwrite the historical R3 artifact. Timing columns
below are medians of per-round measured-window means; pooled P50/P99 use every
latency sample retained by the compact artifact.

### All v2 configurations

| Mode | Clients | Workload | Median QPS | Pooled P50 us | Pooled P99 us | Append mean us | Data-fsync mean us | SYNC mutation mean us | fsync share |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| sync | 1 | write | 687.070 | 1374.345 | 2879.352 | 6.820 | 1154.815 | 1165.759 | 0.990612 |
| sync | 1 | mixed | 2228.074 | 216.531 | 1945.638 | 7.257 | 1119.843 | 1131.017 | 0.990120 |
| sync | 1 | ttl | 1215.932 | 600.650 | 2347.764 | 6.589 | 1109.015 | 1119.335 | 0.990780 |
| sync | 8 | write | 880.425 | 8714.207 | 16553.763 | 6.177 | 1102.107 | 1112.406 | 0.990957 |
| sync | 8 | mixed | 2838.914 | 2385.140 | 10951.652 | 9.964 | 1390.477 | 1404.541 | 0.989882 |
| sync | 8 | ttl | 1096.153 | 5564.772 | 25523.894 | 8.659 | 1719.864 | 1733.624 | 0.992063 |
| sync | 32 | write | 617.673 | 46229.579 | 90371.772 | 9.238 | 1549.798 | 1567.930 | 0.990576 |
| sync | 32 | mixed | 2791.755 | 10698.539 | 30171.530 | 12.628 | 1636.018 | 1655.599 | 0.988173 |
| sync | 32 | ttl | 1105.965 | 27438.059 | 68522.596 | 11.245 | 1702.918 | 1720.882 | 0.989986 |
| interval | 1 | write | 2290.404 | 393.712 | 721.018 | 4.927 | 6761.900 | — | — |
| interval | 1 | mixed | 2319.531 | 370.827 | 703.912 | 9.791 | 9340.650 | — | — |
| interval | 1 | ttl | 2172.389 | 379.212 | 709.066 | 6.452 | 16375.100 | — | — |
| interval | 8 | write | 3206.664 | 1929.780 | 20616.165 | 4.434 | 18811.700 | — | — |
| interval | 8 | mixed | 3235.710 | 1955.815 | 17702.712 | 9.173 | 15936.750 | — | — |
| interval | 8 | ttl | 3370.619 | 1888.606 | 18955.247 | 5.761 | 14968.250 | — | — |
| interval | 32 | write | 3586.213 | 7381.855 | 35754.491 | 4.208 | 14230.500 | — | — |
| interval | 32 | mixed | 3735.053 | 7336.000 | 35570.755 | 8.605 | 10952.900 | — | — |
| interval | 32 | ttl | 3794.925 | 7331.449 | 30518.429 | 5.585 | 11685.750 | — | — |

### R4-A gate evaluation

- **G1 PASS.** The artifact retains the source HEAD, clean-worktree marker,
  environment, seed, round count, all 18 configurations, and the R3 artifact.
- **G2 PASS.** For 32-client `SYNC` write, the median
  `wal_data_fsync_total_us / sync_mutation_total_us` is **0.990576**, above the
  `0.50` threshold. The median per-round data-fsync mean is 1,549.798 us,
  compared with 9.238 us for WAL append and 1,567.930 us for the whole `SYNC`
  persistence path. The median data-fsync count per successful mutation is
  1.0. Pooled P50/P99 are 46,229.579/90,371.772 us at 617.673 median QPS;
  recovery median is 0.022041 s and server RSS peak is 4,612 KiB.
- **G3 PASS.** A separate non-production experiment observed the unmodified
  `SYNC` server across 2,985 successful WAL data-fsync windows. Every round had
  P50/P90 pending count 32, and the pooled fraction with at least eight pending
  requests was **0.987605**, above the frozen thresholds. This is evidence of
  an external batch-formation opportunity, not of a server queue or group
  commit.
- **G4 PASS.** The formal 5-round run was collected on clean implementation
  HEAD `a06d7c2` after GitHub Actions run `43006353520` passed 9/9. Median QPS is
  **2,858.989**, or **6.84x** the frozen `418.018` gate baseline and 4.63x the
  clean v2 instrumentation baseline `617.673`. Pooled P50/P99 are
  **10,476.388/21,839.172 us**, with 29,100/29,100 successful requests, zero
  errors, 6.696833 records per batch on average, and zero queue rejections or
  writer failures. Full resource and per-round data are below.
- **G5/G6 PASS.** The six-configuration 338-test matrix, 24 process cases,
  group-commit fault tests, and bounded-resource assertions pass locally; the
  implementation commit also passed all CI jobs.
- **G7 COMPLETE.** ADR-005 records the gates, ADR-006 records the implementation
  contract and failure matrix, and the G4 artifact plus final status are
  committed with this evidence.

See [ADR-005](decisions/005-group-commit.md) for candidate boundaries and
[ADR-006](decisions/006-group-commit-implementation.md) for the implementation.
The absence of a public `BATCHED` mode is intentional: group commit is an
internal `SYNC` optimization.

## R4-B background full-checkpoint evidence

R4-B removes RDB serialization and durable I/O from the long owner-loop
checkpoint section. A successful SAVE has two short admission cuts: the first
drains accepted mutations, rotates/flushes the WAL, and captures an independent
RdbSnapshot; normal admission then resumes while the checkpoint worker
serializes, writes, fsyncs, renames, and directory-fsyncs the RDB. The final cut
drains writes admitted during that work, publishes a manifest containing the
complete live WAL-segment set beginning at checkpoint_lsn + 1, and schedules
prefix cleanup. Snapshot capture remains O(records) and runs while admission is
paused; this is not a zero-pause design.

### Formal artifact

- bench/results/r4b-background-checkpoint-20261111.json
- 43,519 bytes
- SHA-256
  d562809738df883e6bf9b9bf317569ab9aa66b8cfb7c72b2e5a8cbaec2ba6aa4
- clean source HEAD fbfd6a6667285dd24448c6fa6df7dd1a73790119
- working_tree_dirty_at_collection=false
- harness SHA-256
  e2672c0fcb38adf9d462849b6c2592baeb5ec5f3a4c3f88ba8c5e60a7e70f0b0
- KV binary SHA-256
  295434e3fe00d06f9644a7530edf7b14d0b557a59602a9ff0648949c9e539909
- schema minidb-reliability-benchmark-v3

The retained primary benchmark used 32 closed-loop clients, SYNC, 100%
writes, five rounds of two seconds, 128 keys, and 64-byte values. It recorded
34,916/34,916 successful requests and zero errors. Median QPS was
3,526.336. Pooled P50/P99/P999 latency was
8,794.040/17,403.269/21,444.012 us. Recovery median was 0.043122 s,
data fsyncs per successful SYNC mutation were 0.157958, and the mean batch
size was 6.330813 records. Server RSS peaked at 4,836 KiB.

The checkpoint probe preloaded 22,000 keys with 768-byte values and then ran
five SAVE rounds with 16 competing writers. Writers completed 10,467 requests
and received 233 explicit ERROR 100 backpressure responses. All five
checkpoints succeeded, checkpoint_failures was zero, and the state after each
SAVE was idle.

| Checkpoint phase | Median us |
|---|---:|
| SAVE client latency | 391,583.697 |
| Admission blocked | 27,193 |
| Snapshot capture | 13,207 |
| Snapshot serialize | 331,836 |
| Temporary RDB write | 13,373 |
| RDB fsync | 11,553 |
| Manifest publish | 3,639 |
| WAL cleanup | 4,383 |
| Checkpoint total | 387,509 |

Maximum observed checkpoint total was 447,473 us. The large SAVE duration is
dominated by snapshot serialization, but that work is outside admission-blocked
time. These phase counters use monotonic time from the first admission cut to
completion; barrier scheduling, wakeups, completion delivery, and network
response work need not sum exactly to the end-to-end value.

Limits specific to this probe:

- in_flight_during_save counts client command intervals overlapping the SAVE
  response interval; it is not a strict count of requests blocked by admission.
- The 233 SET returned 100 responses are explicit backpressure/errors, not
  confirmed writes that later disappeared. The primary benchmark recorded zero
  errors.
- The probe uses a synthetic 22,000-record dataset and is not a capacity or SLA
  result.
- The evidence covers process crash consistency, not power loss, kernel crash,
  storage-cache violations, or media corruption.
- It does not establish automatic checkpointing, incremental/COW checkpointing,
  multi-writer operation, or bounded SAVE latency for arbitrary datasets.

## R4-C snapshot-isolation transaction evidence

R4-C replaces the single current-value store view with a per-key version chain
and adds one explicit transaction layer at the owner-loop boundary. The design
details are frozen in ADR-008. The implemented semantics are:

- a transaction fixes `snapshot_lsn` at BEGIN and reads its private write set
  first, then the newest committed version with `commit_lsn <= snapshot_lsn`;
- writes remain private until COMMIT and are never visible as dirty writes;
- first-committer-wins checks both applied key LSNs and accepted-but-not-yet
  applied pending commits, so a queued commit cannot be silently overwritten;
- COMMIT allocates one LSN and emits one WAL v2 `kTxn` record for all keys; the
  record is applied atomically by the owner loop;
- read-only COMMIT, ROLLBACK, and CONFLICT allocate no LSN and write no WAL;
- an accepted COMMIT completes even if its client disconnects, while an
  uncommitted transaction rolls back on disconnect, session destruction, or
  shutdown;
- checkpoint export contains only committed live state and never a private
  transaction write set.

The isolation level is snapshot isolation, not serializable isolation. Write
skew and predicate/range conflicts are explicitly outside the contract. HTTP
`/kv` mutations remain implicit one-operation transactions; there is no
cross-request HTTP transaction handle.

The bounded transaction surface is:

| Resource | Limit |
|---|---:|
| Active transactions | 64 |
| Write keys per transaction | 256 |
| Transaction payload | 1 MiB |
| Global version entries | 1,000,000 |

Protocol transaction opcodes are `BEGIN=9`, `COMMIT=10`, and `ROLLBACK=11`;
write-write conflicts return `CONFLICT=105`. The top-level `transactions` stats
object reports active/started/committed/conflicted/rolled-back/failed counts,
write conflicts, version high-water marks and reclamation, oldest-snapshot lag,
commit records/bytes/latency, and GC latency.

The R4-C local gate is the same six-configuration matrix shown above
(395/395 in each configuration), including 26 process-level cases, plus TSan,
ASan, UBSan, four 20,000-run fuzz targets, formatting, and static analysis. The
committed implementation and evidence commits must each obtain their own 9/9
GitHub Actions result before release status is claimed.

## R4-D durable Agent state backend evidence

R4-D adds a typed durable state layer over the existing committed KV, LSN,
transaction, WAL, RDB, checkpoint, and recovery machinery. The design is frozen
in ADR-009. The implemented surface is:

- binary KV command `AGENT=12` with 15 session/tool/lease/generic subcommands;
- MDAR records and an MDOR response envelope in the reserved namespace
  `\x00minidb-agent-v1\x00`;
- SHA-256 canonical request hashes, 1..128-byte idempotency keys, and replay of
  the original response without a new LSN;
- state mutation and idempotency in one internal `kTxn`, using the normal
  group-commit durability mode;
- strict revision/CAS, session lease TTL, monotonic fence, tool
  claim/complete/requeue, and crash takeover without automatic RUNNING-tool
  rewriting;
- bounded session/tool/generic/idempotency indexes and a 64 MiB logical-byte
  budget;
- roughly one-second owner-loop GC with 64-candidate-group batches and atomic
  session+tool deletion by internal `kTxn`;
- startup validation of the marker and every MDAR, index/count reconstruction,
  orphan-tool rejection, and fail-stop on malformed state;
- HTTP exclusion and reserved-prefix rejection on every ordinary KV/HTTP path;
- a stable top-level `agent` object in both `/stats` and KV `STATS`.

The R4-D local gate uses the six-configuration matrix above (440/440 in each
configuration), including 31 process-level cases. The five Agent process cases
are `AgentIdempotencyCrashRecovery`, `AgentLeaseTakeover`, `AgentCrashRecovery`,
`AgentNamespaceCollision`, and `AgentStatsContract`. The fixed-seed fuzz gate
runs the six 20,000-run targets listed below with an empty artifact directory.
TSan, ASan, UBSan, clang-format, clang-tidy, `git diff --check`, and Python
syntax compilation are part of the same gate.

This evidence covers process crashes and restart, not power loss, multi-node
lease safety, or exactly-once external tool side effects. The evidence commit
`c07b977` obtained its own 9/9 GitHub Actions result in run
[`43101139773`](https://github.com/TXSAJYDN/MiniDB/actions/runs/43101139773);
ADR-009 now records this outcome as Accepted.

## R4-A G3 batch-formation experiment

This experiment is external to the production write path. It starts the
unmodified Release `minidb_kv_server -d sync`, drives 32 closed-loop persistent
KV clients with one in-flight `SET` each, and runs the server under
`strace -f -ttt -T -yy -qq -e trace=fsync`. Client timestamps use
`CLOCK_REALTIME` nanoseconds and are correlated only with successful fsyncs to
WAL segment files.

The primary metric is deliberately conservative:

~~~text
pending_count =
  count(SET where send_completed_ns <= fsync_start_ns
             and acknowledged_ns >= fsync_end_ns)
~~~

It counts requests fully sent before fsync entry that were still unacknowledged
after fsync exit. Each round must reconcile successful requests, measured
`STATS` append/fsync/`SYNC` mutation deltas, and parsed WAL data-fsync windows.
Directory fsyncs, RDB fsyncs, preload, and shutdown checkpoint fsyncs are
excluded.

Artifact:

- `bench/results/r4a-batch-formation-20261111.json`
- 1,435,200 bytes
- SHA-256:
  `f68b485204600ba83d88dd04834ee2df08595776af150cdfb164846ae6b4439d`
- `git_head=5d26f0be1dba0ff7e383d9660999c50204aa831f`
- `working_tree_dirty_at_collection=false`
- harness SHA-256:
  `8eafe2b777de7da6413086a18e84d920375c9ea227132c9df96a98d15b77f629`
- server binary SHA-256:
  `5eb7f8055489bc9bfb792a219bdc7c685760fa4020fe16ff7c15de53c2f60d38`
- frozen policy: 5 rounds, 32 clients, 2.0 s per round, 128 keys,
  64-byte values, seed `20261111`

| Round | Successful SET | Windows | Pending P50 | Pending P90 | `>=8` fraction |
|---:|---:|---:|---:|---:|---:|
| 1 | 597 | 597 | 32 | 32 | 0.988275 |
| 2 | 618 | 618 | 32 | 32 | 0.988673 |
| 3 | 607 | 607 | 32 | 32 | 0.988468 |
| 4 | 576 | 576 | 32 | 32 | 0.986111 |
| 5 | 587 | 587 | 32 | 32 | 0.986371 |

Across all rounds: 2,985 successful `SET`s, 2,985 WAL data-fsync windows,
0 errors; pending P50/P90/P99 all 32, mean 31.146, and 2,948/2,985 =
**0.987605** windows with at least eight pending requests. Every round satisfied
`successes == wal.appends == wal.data_fsyncs == persistence.sync_mutations ==
trace windows`. WAL data-fsync duration was P50 1,898.000 us, P90 2,270.000 us,
and P99 2,716.800 us; client latency was P50 111,990.761 us and P99
133,233.279 us.

The median traced throughput was 283.293 ops/s. Because tracing overhead is
present and the clients are closed-loop, this value is not a G4 performance
result. The experiment does not prove that the production admission queue contains
those exact requests, that writer/completion overhead is negligible, or that
the G4 throughput gate passes.

## R4-A G4 group-commit benefit run

### Configuration and provenance

- Artifact: `bench/results/r4a-group-commit-20261111.json`
- Size: **28,319 bytes**
- SHA-256:
  `0ecf064ed9d7b70971188199abbc1194d974659e5ffa3ac24908b7ca9e8f8f7e`
- Clean source HEAD:
  `a06d7c2f3e0be7fc7da1217c3c6f4c6f04fc3128`
- `working_tree_dirty_at_collection=false`
- Implementation CI:
  [run 43006353520](https://github.com/TXSAJYDN/MiniDB/actions/runs/43006353520),
  9/9 jobs passed before collection
- Rounds: 5, each with 2 measured seconds after warm-up
- Dataset: 128 keys, 64-byte values; seed `20261111`
- Workload: `SYNC` write (KV `SET`), 32 closed-loop persistent clients, one
  in-flight command per client
- Recovery: `SIGKILL` after each measured round, then fresh-process startup to
  `PING`; checkpoint: KV `SAVE` after recovery
- Frozen gate: median QPS `>= 836`, exactly 2x the R3 `SYNC` write baseline
  `418.018`; no threshold or durability semantic was changed

### Comparison with the clean instrumentation baseline

The v2 baseline was collected before the writer existed. It is the fairest
same-machine implementation comparison; the `418.018` R3 number remains the
frozen gate baseline.

| Metric | v2 baseline | G4 | Change |
|---|---:|---:|---:|
| Median QPS | 617.673 | **2,858.989** | 4.63x |
| Pooled P50 / P99 us | 46,229.579 / 90,371.772 | **10,476.388 / 21,839.172** | lower |
| Client / server CPU median, one core | 6.119% / 6.256% | 176.739% / 24.383% | higher |
| Client / server RSS peak KiB | 20,908 / 4,612 | 19,716 / 4,836 | server +224 KiB |
| WAL write amplification | 1.527778 | 1.527778 | unchanged |
| Recovery median s | 0.022041 | 0.044825 | about 2x |
| Data fsyncs per `SYNC` mutation | 1.0 | 0.149324 | about 6.7x fewer |
| Records per batch mean | — | 6.696833 | — |
| Queue rejections / writer failures | — | 0 / 0 | — |

The frozen gate is satisfied: 2,858.989 is **3.42x** the `836` threshold and
**6.84x** the `418.018` gate baseline. Relative to the clean v2 baseline, the
gain is **4.63x**. The throughput and latency improvement is accompanied by
higher client/server CPU use, a 224 KiB increase in peak server RSS, and roughly
twice the measured recovery time. Those costs are part of the result, not
footnotes to omit.

### Per-round observations

| Round | QPS | P50 us | P99 us | Batches | Batch records | Max batch records | Queue rejects | Writer failures |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 2,731.783 | 10,858.455 | 22,696.026 | 851 | 5,615 | 26 | 0 | 0 |
| 2 | 2,786.958 | 10,680.789 | 22,151.359 | 853 | 5,715 | 20 | 0 | 0 |
| 3 | 2,858.989 | 10,482.744 | 21,613.536 | 863 | 5,857 | 28 | 0 | 0 |
| 4 | 2,887.715 | 10,291.577 | 21,297.945 | 884 | 5,920 | 24 | 0 | 0 |
| 5 | 2,925.525 | 10,147.647 | 21,367.014 | 923 | 5,993 | 23 | 0 | 0 |

All 29,100 measured requests succeeded; `attempts=29,100` and `errors=0`.
Median batches and batch records are 863 and 5,857 respectively, so records
outnumber physical batches in every round and the data fsync count equals the
batch count. Actual batches stayed far below the 256-record/16-MiB limits, so
this run exercised normal group formation rather than the maximum-size path.
Median checkpoint pause and save time were 14,587 us and 16,180.828 us.

Interpretation limits:

- the result is from WSL2 loopback, one host, one closed-loop 32-client
  configuration, and the fixed dataset above;
- it is not open-loop saturation capacity or evidence for more than 32 clients;
- every `SYNC` acknowledgement still waited for the data fsync covering its
  LSN; no loss window was added;
- the measured CPU/RSS/recovery costs must accompany the throughput claim.

## R3 reliability benchmark

### Configuration

- `bench/results/reliability-20261111.json`
- 133,299 bytes
- SHA-256:
  `a5c3099789be938654771f830acbd25ecedc1da5520f7c4aa1567493c0e74254`
- 18 configurations
- durability modes: `SYNC`, `INTERVAL`
- clients: 1, 8, 32
- workloads: write, mixed, TTL
- 5 rounds per configuration
- 2 seconds per measured round
- dataset: 128 keys, 64-byte values
- seed: `20261111`
- connection model: closed-loop persistent KV connections, one in-flight
  command per client
- recovery method: `SIGKILL` after the measured round, then fresh-process
  startup to `PING`
- checkpoint method: KV `SAVE` after recovery, measured client-side and via
  `STATS checkpoint_pause_us`

Workload definitions:

- **write**: SET with a 64-byte value;
- **mixed**: 80% GET, 20% SET;
- **TTL**: 50% EXPIRE with a 5-60 second TTL, 50% TTL.

### All configurations

| Mode | Clients | Workload | Median QPS | P99 us | Checkpoint pause median us | Recovery median s | Server RSS max KiB |
|---|---:|---|---:|---:|---:|---:|---:|
| sync | 1 | write | 408.815 | 3,593.020 | 14,015 | 0.023150 | 4,472 |
| sync | 1 | mixed | 1,220.442 | 2,884.838 | 12,344 | 0.023140 | 4,468 |
| sync | 1 | ttl | 794.180 | 2,858.482 | 12,225 | 0.022936 | 4,472 |
| sync | 8 | write | 547.029 | 18,584.584 | 13,751 | 0.022894 | 4,504 |
| sync | 8 | mixed | 2,219.464 | 8,470.219 | 11,571 | 0.023829 | 4,504 |
| sync | 8 | ttl | 1,046.322 | 30,195.330 | 13,017 | 0.023154 | 4,500 |
| sync | 32 | write | 418.018 | 115,463.883 | 14,767 | 0.022716 | 4,608 |
| sync | 32 | mixed | 2,123.741 | 38,182.470 | 13,824 | 0.023171 | 4,608 |
| sync | 32 | ttl | 942.838 | 67,931.036 | 20,852 | 0.022893 | 4,608 |
| interval | 1 | write | 2,129.439 | 844.501 | 17,521 | 0.043730 | 4,468 |
| interval | 1 | mixed | 2,261.862 | 779.012 | 12,437 | 0.044585 | 4,468 |
| interval | 1 | ttl | 2,020.629 | 845.240 | 19,323 | 0.044135 | 4,468 |
| interval | 8 | write | 2,744.619 | 27,461.526 | 14,205 | 0.044332 | 4,504 |
| interval | 8 | mixed | 2,796.337 | 25,324.990 | 13,811 | 0.043771 | 4,504 |
| interval | 8 | ttl | 2,706.751 | 28,776.473 | 12,409 | 0.085165 | 4,504 |
| interval | 32 | write | 3,000.253 | 43,255.135 | 13,148 | 0.086119 | 4,604 |
| interval | 32 | mixed | 3,367.577 | 37,294.600 | 14,068 | 0.043873 | 4,608 |
| interval | 32 | ttl | 3,084.165 | 46,918.476 | 12,387 | 0.044257 | 4,608 |

### Aggregate

| Metric | SYNC | INTERVAL |
|---|---:|---:|
| Median QPS range | 408.815 - 2,219.464 | 2,020.629 - 3,367.577 |
| Recovery median range | 0.022716 - 0.023829 s | 0.043730 - 0.086119 s |
| Checkpoint pause median range | 11,571 - 20,852 us | 12,387 - 19,323 us |
| Server RSS peak max | 4,608 KiB | 4,608 KiB |

Total successful requests: 340,240. Total errors: 0. Wall-clock collection time:
238.703 seconds.

### Interpretation

- `SYNC` pays one WAL data fsync per successful mutation on the owner event
  loop. Its throughput is therefore much lower than `INTERVAL` for write-heavy
  workloads.
- `INTERVAL` acknowledges after append and flushes on a 100 ms timer,
  checkpoint, or shutdown. Its higher throughput comes with an explicit loss
  window.
- The 32-client rows reach the server's hard connection cap. They are a
  boundary observation, not evidence of scaling beyond 32 clients.
- Recovery times here are for small datasets and one quiescent crash point per
  round. They are not a worst-case recovery bound.
- At the time of the historical R3 run, checkpoint pause was stop-the-world
  owner-loop time; R4-B introduced the background checkpoint later.

## What the evidence does not prove

- power-loss or kernel-crash durability;
- durability when the storage device or firmware violates fsync semantics;
- behavior under media corruption;
- production capacity or open-loop saturation;
- multi-node, replicated, serially-isolated, distributed, or multi-writer
  transaction operation;
- any Redis or SQL compatibility;
- a production SLA;
- that the G4 result generalizes to other hardware, power modes, datasets, or
  more than 32 clients;
- that the measured CPU and recovery-time costs are acceptable outside the tested
  configuration;
- that external pending overlap alone proves group-commit correctness or speed;
- that every later commit inherited an earlier CI result without its own checks.

## Reproduction notes

The authoritative commands are the CMake presets in `CMakePresets.json` and the
fuzz/benchmark READMEs. Use a fresh build directory for each configuration.

For TSan on WSL2:

~~~bash
setarch x86_64 -R cmake --build --preset tsan -j8
setarch x86_64 -R ctest --preset tsan --output-on-failure
~~~

For the reliability benchmark, rebuild Release and run
`bench/run_reliability_benchmark.py` with the same seed, round count, client
set, workloads, and a fresh work directory. Compare only runs from the same
machine, power mode, kernel, compiler, build configuration, and background load.

The frozen G4 command was:

~~~bash
python3 bench/run_reliability_benchmark.py \
  --kv build/ci-release/app/minidb_kv_server \
  --rounds 5 --duration-seconds 2 \
  --dataset-keys 128 --value-size 64 \
  --seed 20261111 \
  --modes sync --clients 32 --workloads write \
  --out bench/results/r4a-group-commit-20261111.json
~~~

Use a new output path for reproduction; do not overwrite the retained artifact.

For the frozen G3 batch-formation experiment, first run its self-test, then use
a new output path so the retained artifact is never overwritten:

~~~bash
python3 bench/run_r4a_batch_formation.py --self-test
python3 bench/run_r4a_batch_formation.py \
  --out /tmp/r4a-batch-formation-reproduction.json
~~~

The experiment requires `strace`, a Release `minidb_kv_server`, and a clean
source commit for formal evidence. A failed gate verdict is retained as
negative evidence; it is not converted into a PASS by changing thresholds.

## Related documents

- [Crash consistency](crash-consistency.md)
- [Recovery model](recovery.md)
- [WAL and manifest format](wal-format.md)
- [Deployment and operation](deployment.md)
- [Benchmark methodology](../bench/README.md)
- [ADR-005: R4-A group-commit candidate design and evidence gates](decisions/005-group-commit.md)
- [ADR-006: R4-A group-commit implementation](decisions/006-group-commit-implementation.md)
- [ADR-008: R4-C MVCC and snapshot-isolation transactions](decisions/008-mvcc-transactions.md)
- [ADR-009: R4-D durable Agent state backend](decisions/009-agent-state-backend.md)
- [Fuzz targets](../fuzz/README.md)
