# Benchmark methodology

`bench/benchmark.py` measures one persistent loopback connection per protocol
using only the Python standard library. It is a repeatable workstation baseline,
not a saturation, capacity, or multi-client scaling test.

## Workload

For both HTTP and KV:

1. Start Release server binaries on ephemeral loopback ports with private RDB
   files.
2. Load a fixed dataset with key count and value size supplied on the command
   line.
3. Perform an untimed warm-up.
4. Measure sequential `GET` latency for every requested sample and derive
   throughput from total elapsed wall time.
5. Stop each server with SIGTERM and fail the run unless the save path exits 0.

The default matrix is 256 keys, 256-byte values, 200 warm-up GETs, and 2,000
measured GETs per protocol. The dataset is deterministic; values include binary
bytes and are verified on every measured response.

## Run

Use Release binaries:

~~~bash
python3 bench/benchmark.py \
  --http /home/win11/minidb-final-matrix-v2/release/app/minidb_http_server \
  --kv /home/win11/minidb-final-matrix-v2/release/app/minidb_kv_server \
  --samples 2000 --warmup 200 --keys 256 --value-size 256 \
  --out /home/win11/minidb-final-evidence-v2/benchmark/release-wsl2.json
~~~

The JSON result contains:

- source-like environment metadata recorded by the client;
- dataset, warm-up, sample, and connection-model configuration;
- per-protocol throughput and raw latency samples;
- min/mean/P50/P95/P99/max microseconds;
- explicit limitations.

The raw sample array is part of the evidence. Do not publish only rounded
percentiles.

## Interpretation limits

- WSL2 virtualizes networking and shares host scheduling with other workloads.
- Client and server compete for the same CPU.
- Each protocol uses one connection and one outstanding request at a time.
- Percentiles include Python client parsing and scheduler overhead.
- Results do not demonstrate multi-client scaling, write-heavy throughput,
  durability cost under load, or behavior at the 32-connection limit.
- Compare runs only on the same machine, power mode, kernel, compiler, build
  configuration, and background-load conditions.

The acceptance report must record the exact command, environment, raw JSON
path, and these limitations. A single favorable number is not a capacity claim.
