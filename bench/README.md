# Benchmark methodology

MiniDB keeps four complementary benchmark harnesses:

- `bench/benchmark.py` is the short, deterministic sample-count regression used
  to validate the measurement plumbing;
- `bench/run_reliability_benchmark.py` is the R0-R3 durability, concurrency,
  SIGKILL recovery, and checkpoint-pause harness, the R4-A v2 baseline
  collector, the formal clean-HEAD G4 benefit harness, and the R4-B v3 checkpoint-probe/evidence harness;
- `bench/run_r4a_batch_formation.py` is the external, non-production R4-A G3
  batch-formation experiment;
- `bench/run_acceptance_benchmark.py` is the historical Stage H timed harness,
  retained only for reproducibility of the pre-WAL baseline.

None of these harnesses is a saturation, capacity, open-loop, or
production-SLA test. The R3 reliability benchmark supports current durability
claims; the R4-A v2 baseline supports the diagnostic timing evidence used by
ADR-005; the G3 harness observed external pending-request overlap around real
WAL data fsyncs without changing the production write path; the clean-HEAD G4
run measures the implemented bounded group-commit path in one frozen
configuration; and the v3 run adds the concurrent-writer checkpoint probe used
by ADR-007. Stage H numbers are historical background.

## Reliability benchmark suite

### R3 historical baseline

`bench/results/reliability-20261111.json` is the retained R0-R3 baseline:

- 133,299 bytes;
- SHA-256
  `a5c3099789be938654771f830acbd25ecedc1da5520f7c4aa1567493c0e74254`;
- 18 configurations: `SYNC`/`INTERVAL` x clients 1/8/32 x
  write/mixed/TTL;
- 5 rounds per configuration, 2 seconds per measured round;
- dataset: 128 keys, 64-byte values;
- seed: `20261111`;
- closed-loop persistent KV connections, one in-flight command per client;
- after each measured round: `SIGKILL`, fresh-process recovery to `PING`;
- after recovery: KV `SAVE`, with checkpoint pause measured both client-side
  and through `STATS checkpoint_pause_us`;
- 340,240 successful requests, 0 errors, 238.703 seconds wall-clock collection
  time.

The JSON records `git_head=34dcc79` and
`working_tree_dirty_at_collection=true` because it was collected from the
uncommitted reliability implementation. It is retained honestly and is not
rewritten after commit. Full per-configuration data and interpretation limits
are in [`docs/reliability-evidence.md`](../docs/reliability-evidence.md).

Reproduce the historical baseline with a fresh output path:

~~~bash
python3 bench/run_reliability_benchmark.py \
  --kv build/release/app/minidb_kv_server \
  --rounds 5 --duration-seconds 2 \
  --dataset-keys 128 --value-size 64 \
  --seed 20261111 \
  --modes sync interval \
  --clients 1 8 32 \
  --workloads write mixed ttl \
  --out /tmp/reliability-r3-reproduction.json
~~~

### R4-A instrumentation baseline (v2)

The R4-A-era harness emitted schema `minidb-reliability-benchmark-v2`. Its retained
default artifact is `bench/results/reliability-20261111-r4a-baseline.json`; it must
not overwrite the R3 artifact. The current harness emits v3, described below.
The v2 schema added:

- measured-window deltas for successful WAL append, WAL data-fsync, and
  `SYNC` persistence-path totals;
- fixed WAL data-fsync latency buckets from 50 us through 100 ms plus an
  overflow bucket;
- per-round `wal_data_fsyncs_per_sync_mutation` and
  `data_fsync_share_of_sync_mutation`;
- explicit timing semantics and R4-A evidence metadata.

The counters are diagnostic instruments for
[ADR-005](../docs/decisions/005-group-commit.md), not a production SLO. The
retained v2 baseline was collected before the group-commit writer was
implemented, so it measures the single-mutation `SYNC` path. The current
production path uses the bounded writer described by
[ADR-006](../docs/decisions/006-group-commit-implementation.md); it still
exposes only `SYNC` and `INTERVAL`, not a public `BATCHED` mode. `max` fields
are process-lifetime maxima and include preload/warm-up; mean fields use
measured-window deltas. Fixed buckets bound cardinality but do not provide
per-request traces or arbitrary quantiles.

The retained R4-A instrumentation baseline was collected at clean commit
`964ba84`, before group commit was implemented:

- 310,067 bytes, SHA-256 `55bcad4ead05fcba26e44cecf17e71df6ffd7dba17e54a128398c5b65820fe84`;
- 18 configurations x 5 rounds, 414,688 successful requests, 0 errors,
  229.087 seconds wall-clock collection time;
- G1 **PASS**: all configurations and provenance are present;
- G2 **PASS**: 32-client `SYNC` write fsync share of the `SYNC` persistence path
  has median **0.990576**, above the `0.50` threshold;
- G3 **PASS**: the separate [`run_r4a_batch_formation.py`](#r4-a-g3-batch-formation-experiment)
  experiment observed 2,985 WAL data-fsync windows over 5 rounds, 0 errors, and
  a pooled fraction of **0.987605** with at least eight pending requests; this
  is opportunity evidence, not group-commit implementation or performance.

Run the v2 baseline from a clean source commit:

~~~bash
python3 bench/run_reliability_benchmark.py \
  --kv build/release/app/minidb_kv_server \
  --rounds 5 --duration-seconds 2 \
  --dataset-keys 128 --value-size 64 \
  --seed 20261111 \
  --modes sync interval \
  --clients 1 8 32 \
  --workloads write mixed ttl
~~~

The benchmark reaches the repository's 32-connection cap on its largest
configurations. That is a boundary observation, not evidence of scaling beyond
32 clients or of open-loop capacity.

### R4-A G3 batch-formation experiment

`bench/run_r4a_batch_formation.py` is an external experiment. It starts the
unmodified Release `minidb_kv_server -d sync` under
`strace -f -ttt -T -yy -qq -e trace=fsync`, drives 32 closed-loop persistent KV
clients with one in-flight `SET` each, and correlates client
`CLOCK_REALTIME` timestamps only with successful fsyncs to
`wal-NNNNNNNNNNNNNNNNNNNN.log` files. Directory fsyncs, RDB fsyncs, preload,
and shutdown checkpoint fsyncs are excluded.

The frozen G3 policy is:

| Parameter | Value |
|---|---:|
| Rounds | 5 |
| Clients | 32 |
| Duration per round | 2.0 s |
| Dataset | 128 keys |
| Value size | 64 B |
| Seed | 20261111 |
| Durability mode | `SYNC` |
| Minimum successful `SET` per round | 400 |
| Minimum WAL data-fsync windows per round | 200 |
| Maximum errors | 0 |
| Minimum round pending P50/P90 | 8 / 8 |
| Minimum round `>=8` fraction | 0.80 |
| Minimum pooled `>=8` fraction | 0.90 |

The primary metric is:

~~~text
pending_count =
  count(SET where send_completed_ns <= fsync_start_ns
             and acknowledged_ns >= fsync_end_ns)
~~~

Each round must reconcile successful requests, measured `STATS`
`wal.appends`, `wal.data_fsyncs`, `persistence.sync_mutations`, and parsed WAL
data-fsync windows. A formal run requires a clean, committed source HEAD and
must not overwrite an existing artifact.

Retained result:

- `bench/results/r4a-batch-formation-20261111.json`
- 1,435,200 bytes
- SHA-256:
  `f68b485204600ba83d88dd04834ee2df08595776af150cdfb164846ae6b4439d`
- `git_head=5d26f0be1dba0ff7e383d9660999c50204aa831f`
- clean worktree at collection
- harness SHA-256:
  `8eafe2b777de7da6413086a18e84d920375c9ea227132c9df96a98d15b77f629`
- binary SHA-256:
  `5eb7f8055489bc9bfb792a219bdc7c685760fa4020fe16ff7c15de53c2f60d38`
- verdict: **PASS**

| Round | Successful SET | Windows | Pending P50 | Pending P90 | `>=8` fraction |
|---:|---:|---:|---:|---:|---:|
| 1 | 597 | 597 | 32 | 32 | 0.988275 |
| 2 | 618 | 618 | 32 | 32 | 0.988673 |
| 3 | 607 | 607 | 32 | 32 | 0.988468 |
| 4 | 576 | 576 | 32 | 32 | 0.986111 |
| 5 | 587 | 587 | 32 | 32 | 0.986371 |

Pooled: 2,985 successful `SET`s and 2,985 windows, 0 errors; pending P50/P90/P99
all 32; 2,948/2,985 = **0.987605** windows had at least eight pending requests.
WAL data-fsync duration was P50 1,898.000 us, P90 2,270.000 us, and P99
2,716.800 us. The traced median throughput of 283.293 ops/s is not a G4 result.

Reproduce from a clean Release build with a new output path:

~~~bash
python3 bench/run_r4a_batch_formation.py --self-test
python3 bench/run_r4a_batch_formation.py \
  --kv build/release/app/minidb_kv_server \
  --out /tmp/r4a-batch-formation-reproduction.json
~~~

The experiment itself does not add or test a writer thread, queue, batch,
completion, `BATCHED` mode, G4 benefit, G5 crash correctness, or G6
bounded-resource behavior. Passing G3 was only the prerequisite for the
implementation-level [ADR-006](../docs/decisions/006-group-commit-implementation.md);
the implemented path and its G4 result are recorded below.

### R4-A G4 group-commit benefit run

The formal G4 run measured the implemented bounded writer through the production
`SYNC` path. It did not enable a public `BATCHED` mode or change the durability
contract. Retained evidence:

- `bench/results/r4a-group-commit-20261111.json`
- 28,319 bytes
- SHA-256:
  `0ecf064ed9d7b70971188199abbc1194d974659e5ffa3ac24908b7ca9e8f8f7e`
- `git_head=a06d7c2f3e0be7fc7da1217c3c6f4c6f04fc3128`
- clean worktree at collection
- implementation CI: GitHub Actions run
  [43006353520](https://github.com/TXSAJYDN/MiniDB/actions/runs/43006353520),
  9/9 jobs passed before collection
- verdict: **PASS**

The frozen G4 policy was:

| Parameter | Value |
|---|---:|
| Rounds | 5 |
| Clients | 32 |
| Duration per round | 2.0 s |
| Dataset | 128 keys |
| Value size | 64 B |
| Seed | 20261111 |
| Durability mode | `SYNC` |
| Workload | 100% write (`SET`) |
| Frozen R3 baseline | 418.018 ops/s |
| Required median QPS | >= 836 |
| Achieved median QPS | **2,858.989** |
| Relative to baseline | **6.84x** |
| Errors | 0 / 29,100 requests |

| Round | QPS | P50 us | P99 us | Batches | Batch records | Max batch records |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 2,731.783 | 10,858.455 | 22,696.026 | 851 | 5,615 | 26 |
| 2 | 2,786.958 | 10,680.789 | 22,151.359 | 853 | 5,715 | 20 |
| 3 | 2,858.989 | 10,482.744 | 21,613.536 | 863 | 5,857 | 28 |
| 4 | 2,887.715 | 10,291.577 | 21,297.945 | 884 | 5,920 | 24 |
| 5 | 2,925.525 | 10,147.647 | 21,367.014 | 923 | 5,993 | 23 |

Pooled P50/P99 were **10,476.388/21,839.172 us**. Median throughput used
**6.696833 records per batch**; queue rejections and writer failures were both
zero. Relative to the clean v2 instrumentation baseline of 617.673 QPS, the gain
was **4.63x**, but the same run also used more CPU, added 224 KiB to peak server
RSS, and measured about twice the recovery time. Those costs are part of the
result.

Reproduce with a new output path after building Release:

~~~bash
python3 bench/run_reliability_benchmark.py \
  --kv build/ci-release/app/minidb_kv_server \
  --rounds 5 --duration-seconds 2 \
  --dataset-keys 128 --value-size 64 \
  --seed 20261111 \
  --modes sync --clients 32 --workloads write \
  --out /tmp/r4a-group-commit-reproduction.json
~~~

The run is WSL2 loopback, single-node, and closed-loop with one in-flight command
per client. It is not open-loop saturation capacity evidence and does not
generalize beyond the measured 32-client configuration. Every `SYNC`
acknowledgement still waited for the data fsync covering its LSN.

### R4-B v3 background-checkpoint evidence

Schema minidb-reliability-benchmark-v3 keeps the v2 timing and group-commit
fields and adds:

- a checkpoint probe that preloads a larger dataset, starts competing writers,
  and issues five SAVE operations;
- per-phase checkpoint timings for admission blocked, snapshot capture,
  serialize, temporary-file write, RDB fsync, manifest publication, WAL
  cleanup, and total duration;
- client-observed SAVE latency, checkpoint failure count, and the
  background-checkpoint state after each round;
- probe reconciliation data: writer successes, explicit writer errors, and
  client command intervals overlapping the SAVE response window.

The retained formal artifact is:

- bench/results/r4b-background-checkpoint-20261111.json
- 43,519 bytes
- SHA-256
  d562809738df883e6bf9b9bf317569ab9aa66b8cfb7c72b2e5a8cbaec2ba6aa4
- clean source HEAD fbfd6a6667285dd24448c6fa6df7dd1a73790119
- harness SHA-256
  e2672c0fcb38adf9d462849b6c2592baeb5ec5f3a4c3f88ba8c5e60a7e70f0b0
- KV binary SHA-256
  295434e3fe00d06f9644a7530edf7b14d0b557a59602a9ff0648949c9e539909

The primary benchmark is 32 clients, SYNC, 100% writes, five rounds of two
seconds, 128 keys, and 64-byte values. It recorded 34,916/34,916 successful
requests, zero errors, median QPS 3,526.336, and pooled P50/P99/P999
8,794.040/17,403.269/21,444.012 us.

The checkpoint probe uses 22,000 records, 768-byte values, 16 writers, and five
rounds. It recorded 10,467 writer successes, 233 explicit ERROR 100
backpressure responses, zero checkpoint failures, and background state idle
after every round.

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

Maximum checkpoint total was 447,473 us. The in_flight_during_save field counts
client command intervals overlapping the SAVE response interval, not a strict
admission-block count. The 233 rejected writes are explicit backpressure, not
acknowledged writes that disappeared. The probe is a synthetic workload and
does not prove a capacity limit, production SLA, power-loss durability, or a
bounded SAVE time for arbitrary datasets.

Reproduce on a clean Release build with a new output path:

~~~bash
python3 bench/run_reliability_benchmark.py \
  --kv build/ci-release/app/minidb_kv_server \
  --rounds 5 --duration-seconds 2 --dataset-keys 128 --value-size 64 \
  --seed 20261111 --modes sync --clients 32 --workloads write \
  --checkpoint-probe-keys 22000 --checkpoint-probe-value-size 768 \
  --checkpoint-probe-writers 16 --checkpoint-probe-rounds 5 \
  --out /tmp/r4b-background-checkpoint-reproduction.json
~~~

The exact option names and schema are defined by the checked-in harness and its
self-test; always verify the output provenance and digest instead of trusting a
filename.

## Historical Stage H harness

For HTTP and KV the timed harness:

1. starts a Release server on an ephemeral loopback port with a private RDB;
2. loads a fixed 256-key, 256-byte binary-safe dataset and verifies every value;
3. warms up for 10 seconds, measures for 30 seconds, then cools down for
   5 seconds;
4. repeats that cycle for five rounds per protocol;
5. opens one persistent connection at the start of each round and keeps at most
   one GET in flight on it; the connection is re-established between rounds;
6. derives throughput from successful requests divided by measured elapsed
   time and percentiles from every raw latency sample;
7. samples client and server `/proc` counters every 250 ms;
8. stops each server with SIGTERM and fails unless the graceful save exits 0.

The request order is deterministic round-robin over the fixed dataset. Latency
uses the monotonic `time.perf_counter_ns()` clock. Percentiles use linear
interpolation at rank `p*(n-1)` over all raw samples.

## Run the historical Stage H harness

Use Release binaries:

~~~bash
python3 bench/run_acceptance_benchmark.py \
  --http /home/win11/minidb-ah-final-20261106-v3-matrix/release/app/minidb_http_server \
  --kv /home/win11/minidb-ah-final-20261106-v3-matrix/release/app/minidb_kv_server \
  --rounds 5 --warmup-seconds 10 --duration-seconds 30 --cooldown-seconds 5 \
  --resource-interval-ms 250 --keys 256 --value-size 256 \
  --work-dir /home/win11/minidb-ah-final-20261106-v3-evidence/stage-h/timed-work \
  --out /home/win11/minidb-ah-final-20261106-v3-evidence/stage-h/timed-multi-round.json
~~~

The earlier 200-warmup/2,000-measured run remains available as
`stage-h/release-wsl2.json` for short tool regression only. It is not the
default Stage H performance evidence.

The JSON result contains:

- source-like environment metadata recorded by the client, including the Git
  HEAD and porcelain status at collection time;
- dataset, warm-up, round, connection, clock, percentile, and resource-sampling
  configuration;
- per-round and aggregate throughput plus every raw latency sample;
- min/mean/P50/P95/P99/max microseconds;
- client and server CPU, RSS, VmHWM, fd, thread, and context-switch samples;
- explicit limitations.

Do not publish only rounded percentiles. Preserve the raw JSON and its digest.

### Historical Stage H evidence

The accepted run was collected on 2026-11-06 at Git HEAD
`8387714664a86a2e279de5f26f32f4ac75c3c284`. At that moment
`bench/run_acceptance_benchmark.py` was still untracked; the exact script
snapshot and SHA-256 are retained with the evidence. The product code was
unchanged from the accepted `9da4b48` baseline.

Primary evidence:

- `/home/win11/minidb-ah-final-20261106-v3-evidence/stage-h/timed-multi-round.json`
- bytes: `11,780,123`
- SHA-256: `8cfd8a89e6a73b70cc9abb6a4b180f01589bb1792b30c2a21f8c5393ba35afa8`
- validation: `VALIDATION_OK`
- harness SHA-256: `96e4f3aa754c0e409165755aa75463dc0750fc8f1e6f5de8d5f7f273b5538cf5`
- wall clock: `8:08.75`, process exit 0

| Protocol | Requests | QPS | P50 us | P95 us | P99 us | Max us | Errors | Timeouts |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| HTTP | 267,270 | 1781.792 | 546.273 | 752.920 | 898.507 | 2640.141 | 0 | 0 |
| KV | 295,147 | 1967.630 | 492.960 | 691.400 | 825.103 | 2811.275 | 0 | 0 |

Per-round QPS / P99:

| Round | HTTP QPS | HTTP P99 us | KV QPS | KV P99 us |
|---:|---:|---:|---:|---:|
| 1 | 1723.962 | 926.233 | 2008.313 | 818.153 |
| 2 | 1699.289 | 915.857 | 1892.556 | 845.263 |
| 3 | 2008.593 | 814.319 | 2047.580 | 813.065 |
| 4 | 1744.462 | 908.298 | 1976.461 | 819.737 |
| 5 | 1732.653 | 909.639 | 1913.241 | 828.959 |

Resource maxima and CPU means (one-core percentages):

| Protocol | Client CPU mean | Server CPU mean | Client RSS/VmHWM | Server RSS/VmHWM | fd/threads client, server |
|---|---:|---:|---:|---:|---:|
| HTTP | 16.000% | 14.137% | 30,060 / 32,128 KiB | 3,760 / 4,268 KiB | 9/3, 10/2 |
| KV | 14.729% | 11.899% | 41,700 / 44,072 KiB | 3,804 / 4,184 KiB | 9/3, 10/2 |

Both servers saved 256 keys; each RDB is 71,952 bytes with SHA-256
`5328a91eb1cf8f9bf748a65ceaa8809d944104334ee891127d0301118d544c14`.
No `.tmp.*` files remained.

An external `hey 0.1.4` cross-check (3-second warmup, 30-second measurement)
recorded 1,774.774 req/s at `c1`, 14,638.639 req/s at `c10`, and 28,598.660
req/s at `c32`, with every response HTTP 200. At `c32` the `hey` client used
480% CPU and the server reached the repository's 32-connection limit; this is
not evidence of scaling beyond 32 connections or of a server-side capacity
ceiling.

## Interpretation limits

These limits apply to the historical Stage H harness and the reliability
benchmark suite. Each retained artifact additionally states its own limits.

- WSL2 virtualizes networking and shares host scheduling with other workloads.
- Client and server compete for the same CPU, and Python client parsing is
  included in latency.
- Each round is closed-loop: one persistent connection and one outstanding GET.
- Results do not demonstrate open-loop capacity, saturation, multi-client
  scaling, write-heavy throughput, or durability cost under load.
- The `hey` client is CPU-heavy at higher concurrency; `c32` is a connection
  boundary, not a scalability claim.
- Application payload rate is not a full network-bandwidth measurement.
- Hardware performance counters are unavailable on this WSL2 host, so no
  cycles/instructions/branches/cache-miss or flame-graph claims are made.
- Compare runs only on the same machine, power mode, kernel, compiler, build
  configuration, and background-load conditions.
