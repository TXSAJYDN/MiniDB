# Reliability evidence

## Scope

This document records the local R0-R3 evidence for MiniDB's crash-consistent
persistence work. It is deliberately a statement about a tested engineering
prototype, not a production qualification.

The evidence covers:

- WAL, manifest, checkpoint, recovery, and single-writer lock correctness;
- deterministic crash points and process-level crash recovery;
- sanitizer, fuzz, format, and static-analysis gates;
- durability-mode performance, checkpoint pause, recovery time, and RSS;
- explicit limits and non-claims.

## Source and environment

- Date: 2026-11-11 (Asia/Shanghai)
- Source worktree: `/home/win11/projects/miniDB`
- Baseline commit at the start of the upgrade: `34dcc79`
- Environment: Ubuntu 24.04 WSL2, kernel `6.18.58.1-microsoft-standard-WSL2`,
  x86_64, GCC 13.3.0, CMake 3.28.3, Python 3.12.3
- CPU: Intel Core i9-14900HX, 32 logical CPUs visible to WSL2
- Filesystem: WSL2 ext4

The benchmark JSON records `git_head=34dcc79` and
`working_tree_dirty_at_collection=true`, because the reliability implementation
was still uncommitted when the data was collected. The final commit packages the
same implementation and documentation, but the benchmark metadata is retained
honestly rather than rewritten.

## Test matrix

The final local matrix runs each configuration from its own CMake preset:

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
run. This work was committed locally and not pushed, so no claim is made that
GitHub Actions ran on the final commit.

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

## Reliability benchmark

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
- that GitHub Actions ran on the final commit.

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
- [Fuzz targets](../fuzz/README.md)
