# Benchmark methodology

MiniDB keeps three complementary benchmark harnesses:

- `bench/benchmark.py` is the short, deterministic sample-count regression used
  to validate the measurement plumbing;
- `bench/run_reliability_benchmark.py` is the current R0-R3 durability,
  concurrency, SIGKILL recovery, and checkpoint-pause harness;
- `bench/run_acceptance_benchmark.py` is the historical Stage H timed harness,
  retained only for reproducibility of the pre-WAL baseline.

None of these harnesses is a saturation, capacity, open-loop, or
production-SLA test. Only the R3 reliability benchmark supports current
durability claims; Stage H numbers are historical background.

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

The current harness emits schema `minidb-reliability-benchmark-v2`. Its default
artifact is `bench/results/reliability-20261111-r4a-baseline.json`; it must not
overwrite the R3 artifact. The v2 schema adds:

- measured-window deltas for successful WAL append, WAL data-fsync, and
  `SYNC` persistence-path totals;
- fixed WAL data-fsync latency buckets from 50 us through 100 ms plus an
  overflow bucket;
- per-round `wal_data_fsyncs_per_sync_mutation` and
  `data_fsync_share_of_sync_mutation`;
- explicit timing semantics and R4-A evidence metadata.

The counters are diagnostic instruments for
[ADR-005](../docs/decisions/005-group-commit.md), not a production SLO. They do
not implement group commit, a writer thread, a queue, batching, or `BATCHED`.
`max` fields are process-lifetime maxima and include preload/warm-up; mean
fields use measured-window deltas. Fixed buckets bound cardinality but do not
provide per-request traces or arbitrary quantiles.

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

These limits apply to both the historical Stage H harness and the current R3
reliability benchmark. The R3 artifact additionally states its own limits.

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
