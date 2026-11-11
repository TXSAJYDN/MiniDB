# Reliability evidence

## Scope

This document records the local R0-R3 evidence and the R4-A instrumentation
baseline for MiniDB's crash-consistent persistence work. It is deliberately a
statement about a tested engineering prototype, not a production qualification.

The evidence covers:

- WAL, manifest, checkpoint, recovery, and single-writer lock correctness;
- deterministic crash points and process-level crash recovery;
- sanitizer, fuzz, format, and static-analysis gates;
- durability-mode performance, checkpoint pause, recovery time, and RSS;
- R4-A diagnostic timing fields and fixed WAL data-fsync buckets;
- explicit limits and non-claims.

## Source and environment

- Date: 2026-11-11 (Asia/Shanghai)
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

The final local matrix for R0-R3 plus R4-A instrumentation runs each
configuration from its own CMake preset:

| Configuration | CTest |
|---|---:|
| Debug | 305/305 |
| Release | 305/305 |
| ASan | 305/305 |
| UBSan | 305/305 |
| ASan + UBSan | 305/305 |
| TSan (`setarch x86_64 -R`) | 305/305 |

Each configuration includes the 19 `ProcessIntegrationTest.*` cases:

~~~text
CliContract
CliDurabilityContract
HttpPersistence
KvPersistence
TtlScannerLifecycle
CorruptRdbStartup
SaveFailureExit
HttpSyncCrashRecovery
KvSyncCrashRecovery
IntervalFlushRecovery
WriterLockConflict
WalTailTruncation
ManifestCorruption
MissingWalSegment
WalMiddleCorruption
LegacyImport
RandomKillSeed
TtlCrashRecovery
StatsContract
~~~

The unit and integration inventory also includes:

- WAL record codec and CRC32C boundaries;
- WAL manager append, short write, EINTR, fsync, rotation, cleanup, and limits;
- checkpoint manifest round-trip, truncation, ordering, and atomic publication;
- recovery selection, manifest-missing cases, tail truncation, middle
  corruption, LSN continuity, legacy import, and TTL replay;
- persistence manager `SYNC`/`INTERVAL`, checkpoint retry, fail-stop, and
  single-writer lock;
- C01-C14 deterministic crash points.

This is local reproducibility evidence. It is not a substitute for a public CI
run. The R0-R3 baseline commit `cfbda60` passed 9/9 GitHub Actions jobs; R4-A
final-HEAD status must be read from the GitHub checks for that commit.

## Fuzz evidence

Four Clang libFuzzer targets are built with ASan and UBSan:

| Target | Corpus | Runs | Seed |
|---|---|---:|---:|
| `minidb_http_parser_fuzz` | `fuzz/corpus/http` | 20,000 | 20261111 |
| `minidb_kv_protocol_fuzz` | `fuzz/corpus/kv` | 20,000 | 20261111 |
| `minidb_wal_record_fuzz` | `fuzz/corpus/wal` | 20,000 | 20261111 |
| `minidb_checkpoint_manifest_fuzz` | `fuzz/corpus/manifest` | 20,000 | 20261111 |

A clean run requires exit code 0 for every target, no ASan/UBSan diagnostic, and
an empty artifact directory. Any crash must be minimized, added to the corpus,
and covered by a durable regression test before the run is accepted again.

## R4-A instrumentation baseline

R4-A is evidence-gated. This phase adds instrumentation, not group-commit
implementation. The write path still performs one WAL data fsync per successful
`SYNC` mutation on the owner event loop.

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
quantiles or per-request traces. ADR-005 defines G1-G7; no group-commit work
starts unless the baseline and a separate batch-formation experiment satisfy
the gates.

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
- **G3 NOT TESTED.** The 32-client rows show contention and connection-cap
  behavior, not evidence that at least eight consecutive records can be formed
  per fsync window. A separate non-production batch-formation experiment is
  still required.
- **G4-G7 NOT STARTED.** No writer, queue, batch, completion, or `BATCHED`
  mode exists. An implementation-level ADR is required only after G3 passes.

See [ADR-005](decisions/005-group-commit.md) for the candidate boundaries and
remaining gates.

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
- Checkpoint pause is stop-the-world owner-loop time; no background checkpoint
  exists.

## What the evidence does not prove

- power-loss or kernel-crash durability;
- durability when the storage device or firmware violates fsync semantics;
- behavior under media corruption;
- production capacity or open-loop saturation;
- multi-node, replicated, transactional, or multi-writer operation;
- any Redis or SQL compatibility;
- a production SLA;
- group-commit implementation or performance benefit;
- that every later commit inherited the baseline CI result without its own
  checks.

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

## Related documents

- [Crash consistency](crash-consistency.md)
- [Recovery model](recovery.md)
- [WAL and manifest format](wal-format.md)
- [Deployment and operation](deployment.md)
- [Benchmark methodology](../bench/README.md)
- [ADR-005: R4-A group-commit candidate design and evidence gates](decisions/005-group-commit.md)
- [Fuzz targets](../fuzz/README.md)
