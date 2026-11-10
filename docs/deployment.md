# Deployment and operation

## Status and supported environment

MiniDB is a crash-consistent single-node key/value store for bounded-resource
environments. It is a reliability engineering prototype, not a production
service. It does not provide a production SLA, high-concurrency capacity, SQL or
Redis compatibility, or replication.

The verified environment is Ubuntu 24.04 under WSL2 with GCC 13 or Clang 18.
Other Linux distributions may work, but should be revalidated, especially TSan
and performance measurements. Build and run from a Linux filesystem; a Windows
9p copy is not authoritative for builds, tests, durability tests, or benchmark
results.

## Prerequisites

- Linux with epoll, eventfd, timerfd, and signalfd.
- CMake 3.14 or newer.
- A C++20 compiler; GCC 13.3 and Clang 18.1 are the verified versions.
- Python 3.10+ for process tests, benchmark, and tooling.
- Network access for the first GoogleTest FetchContent download, or a local
  GoogleTest source directory.
- Optional: clang-format, clang-tidy, libFuzzer, strace, and perf.

## Build a release binary

~~~bash
cmake --preset release
cmake --build --preset release -j8
ctest --preset release --output-on-failure
~~~

The resulting executables are:

~~~text
build/release/app/minidb_http_server
build/release/app/minidb_kv_server
~~~

Use `cmake --preset debug`, `asan`, `ubsan`, `asan-ubsan`, or `tsan` for the
other local acceptance configurations. TSan must be launched through
`setarch x86_64 -R` in this WSL2 environment.

## Install layout

A simple deployment can use:

~~~text
/opt/minidb/bin/minidb_http_server
/opt/minidb/bin/minidb_kv_server
/var/lib/minidb/http/minidb.rdb
/var/lib/minidb/kv/minidb.rdb
/etc/minidb/
~~~

Run each process under an unprivileged service account. The account needs write
access to the persistence base path's parent directory: the RDB, manifest, lock
file, and WAL directory are all created next to the `-r` path. It also needs
read access to an existing image and execute permission on parent directories.

The two servers must use different persistence base paths. Each process takes an
advisory exclusive `flock` on `<rdb_path>.lock`; a second process using the same
path exits nonzero.

## Persistence layout

Given `-r /var/lib/minidb/kv/minidb.rdb`:

~~~text
/var/lib/minidb/kv/minidb.rdb
/var/lib/minidb/kv/minidb.rdb.manifest
/var/lib/minidb/kv/minidb.rdb.wal/
/var/lib/minidb/kv/minidb.rdb.lock
~~~

- the RDB file is the latest full checkpoint;
- the manifest records the checkpoint LSN and the first retained WAL segment;
- the WAL directory holds the logical mutations after that checkpoint;
- the lock file is held for the process lifetime.

A backup that copies only the RDB is not a current-state backup unless the
process is stopped and the entire set is copied consistently.

## Command line

Both programs share the same option parser:

~~~text
-p <port>                  listening port
-r <path>                  persistence base path
-d <sync|interval>         durability mode (default: sync)
-h                         help; must be used alone
~~~

Defaults:

| Program | Port | RDB path | Durability |
|---|---:|---|---|
| minidb_http_server | 8080 | ./minidb.rdb | sync |
| minidb_kv_server | 6379 | ./minidb.rdb | sync |

Duplicate options, unknown options, positional arguments, empty values,
non-canonical decimal ports, ports outside 1..65535, and invalid durability
values are rejected with exit code 2. Use an absolute persistence path in a
service unit so the working directory cannot change startup behavior.

Examples:

~~~bash
/opt/minidb/bin/minidb_http_server \
  -p 8080 -r /var/lib/minidb/http/minidb.rdb -d sync

/opt/minidb/bin/minidb_kv_server \
  -p 6379 -r /var/lib/minidb/kv/minidb.rdb -d interval
~~~

## Durability modes

| Mode | Success response | Acknowledged-write guarantee | Allowed loss |
|---|---|---|---|
| `SYNC` | WAL append and file fsync succeeded, mutation applied | Process-crash recovery retains every acknowledged write under the tested file/device model | None is intentionally allowed for acknowledged writes |
| `INTERVAL` | WAL append succeeded, mutation applied | Flush window is bounded by the 100 ms timer and I/O scheduling | Recently acknowledged writes before the next flush |

`INTERVAL` flushes on a 100 ms owner-loop timer, before every checkpoint, and
before graceful shutdown. `BATCHED` is not implemented; group commit is an
unstarted R4 option.

Durability semantics cover process crashes and restarts. They do not prove
power-loss, kernel-crash, firmware-cache, or media-failure durability.

## Systemd examples

HTTP unit:

~~~ini
[Unit]
Description=MiniDB HTTP server
After=network.target

[Service]
Type=simple
User=minidb
Group=minidb
WorkingDirectory=/var/lib/minidb/http
ExecStart=/opt/minidb/bin/minidb_http_server -p 8080 -r /var/lib/minidb/http/minidb.rdb -d sync
Restart=on-failure
RestartSec=2
KillSignal=SIGTERM
TimeoutStopSec=120
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
~~~

KV unit is the same shape with:

~~~ini
ExecStart=/opt/minidb/bin/minidb_kv_server -p 6379 -r /var/lib/minidb/kv/minidb.rdb -d sync
~~~

SIGTERM starts graceful shutdown and a final checkpoint. A nonzero exit from the
checkpoint path marks the unit failed. Do not use SIGKILL as the normal stop
mechanism; it bypasses the final checkpoint. Recovery still uses the WAL, so a
`SYNC` acknowledged write should survive, but shutdown is the supported path.

## Startup validation and recovery

Before accepting clients, the process:

1. validates its command line;
2. blocks SIGINT/SIGTERM on the owner thread;
3. binds the listening socket;
4. acquires `<rdb_path>.lock`;
5. recovers the RDB, manifest, and WAL into a temporary store;
6. starts listening only after the complete image validates.

The only empty-start case is a missing RDB with no manifest and no WAL. An
existing RDB without WAL sidecars is imported as a legacy image and bootstrapped
on the first write or checkpoint.

A missing manifest with existing WAL segments is refused rather than guessed. A
manifest without its RDB, a missing referenced segment, a middle-of-file WAL
corruption, a bad checksum, an LSN gap, or a resource-limit breach is an error.
The process exits nonzero, does not listen, and does not overwrite the damaged
image. The only permitted repair is truncating an incomplete record at the tail
of the final WAL segment.

Check startup logs and exit status:

~~~bash
/opt/minidb/bin/minidb_http_server \
  -p 18080 -r /tmp/minidb-http/minidb.rdb -d sync
curl -fsS http://127.0.0.1:18080/health
curl -fsS http://127.0.0.1:18080/stats
~~~

## Health, stats, and smoke checks

HTTP:

~~~bash
curl -i http://127.0.0.1:8080/health
curl -i -X POST --data-binary 'value' http://127.0.0.1:8080/kv/example
curl -i http://127.0.0.1:8080/kv/example
curl -i -X DELETE http://127.0.0.1:8080/kv/example
curl -i -X POST http://127.0.0.1:8080/save
curl -i http://127.0.0.1:8080/stats
~~~

KV:

~~~bash
python3 test_kv_client.py 6379
~~~

A successful `/save` or `SAVE` response means the checkpoint completed: WAL was
flushed, the snapshot was fsynced and atomically renamed, the manifest was
durably published, and old WAL cleanup was attempted. A directory-fsync failure
after a rename is reported as durability unconfirmed and the process exits
nonzero on shutdown.

`/stats` and `STATS` expose a coherent snapshot of the durability mode, LSNs,
WAL bytes and segment count, mutation/fsync/checkpoint counters, checkpoint
pause time, and recovery statistics. They are diagnostic endpoints, not a
health-check substitute.

## Backup

To take a consistent backup, either stop the owning server or copy the complete
set while the process is quiescent:

~~~bash
systemctl stop minidb-kv
backup_dir="/var/backups/minidb/kv-$(date +%Y%m%dT%H%M%S)"
install -d -m 0700 "$backup_dir"
cp -a /var/lib/minidb/kv/minidb.rdb \
      /var/lib/minidb/kv/minidb.rdb.manifest \
      /var/lib/minidb/kv/minidb.rdb.wal \
      "$backup_dir/"
systemctl start minidb-kv
~~~

Do not copy only the RDB while the process is running. Do not copy a temporary
`.tmp.*` file. Do not copy the `.lock` file as part of the data set; it is only
an ownership marker.

If an external snapshot mechanism is used, it must observe a consistent point
across the RDB, manifest, and WAL directory. A crash-consistent copy of the
whole set can be recovered by the normal startup path.

## Restore

1. Stop the owning server.
2. Preserve the current persistence set for diagnosis.
3. Put a known-good RDB, manifest, and WAL directory at the configured base
   path with correct ownership and permissions.
4. Start the server in the foreground and confirm it recovers without error.
5. Run HTTP health or KV PING, then a read smoke test.

~~~bash
systemctl stop minidb-kv
cp -a /var/backups/minidb/kv-<timestamp>/. /var/lib/minidb/kv/
chown -R minidb:minidb /var/lib/minidb/kv
systemctl start minidb-kv
~~~

If recovery fails, startup must fail rather than silently starting empty. Do not
delete the only copy to force startup.

## Capacity and resource planning

Hard defaults are documented in the protocol, WAL, and RDB references.
Operationally:

- at most 32 concurrent TCP connections per process;
- 1.25 MiB input and output budgets per connection;
- 1 MiB maximum values and 1 KiB maximum keys;
- 100,000 RDB records and 64 MiB maximum RDB file;
- 16 MiB WAL segment, 256 MiB total WAL, 128 segments;
- 1,000,000 recovered records and 512 MiB recovered WAL bytes;
- owner-loop processing, so a synchronous checkpoint blocks new requests.

WAL growth triggers a synchronous checkpoint before the segment budget is
exhausted. If that checkpoint fails, the mutation is rejected and persistence
may fail-stop. Set an appropriate service file-descriptor limit and keep the
data directory on a filesystem with reliable fsync and rename semantics.

## Security

MiniDB has no TLS, authentication, or authorization. Bind it only to a trusted
interface or protect it with an external network boundary. Anyone who can reach
the HTTP or KV port can read and modify all keys and can request a checkpoint.

Do not expose the KV port to the public internet. Do not run the server as root.
Restrict the persistence directory to the service account because it contains
all data.

## Upgrade and rollback

WAL v1, manifest v1, and RDB v2 are the only accepted formats. Before replacing
a binary:

1. run the target build's full local tests;
2. back up the complete RDB + manifest + WAL set;
3. stop the old process gracefully;
4. install the new binary;
5. start it against a copy of deployment data in a staging directory first;
6. verify recovery and read/write behavior;
7. retain the previous binary and persistence set for rollback.

There is no online migration or rolling upgrade path.

## Shutdown behavior

SIGINT and SIGTERM are delivered through signalfd to the owner loop. After the
loop exits, the server joins the TTL scanner and performs a final checkpoint.
Exit code 0 requires `RdbSaveResult::Ok`. A `Failed` or `DurabilityUnconfirmed`
result returns nonzero.

The correct unit stop sequence is therefore:

~~~bash
systemctl stop minidb-kv
systemctl status minidb-kv
~~~

Do not assume that a visible new RDB file alone proves crash durability. Follow
the process exit code and the log message, and inspect `/stats` when diagnosing a
running process.

## Deployment acceptance checklist

- [ ] Release binary built from the intended source digest.
- [ ] Configuration uses absolute paths and distinct persistence base paths.
- [ ] Service runs as an unprivileged account.
- [ ] Parent directory is writable and fsync-capable.
- [ ] Writer-lock conflict is tested and exits nonzero.
- [ ] Missing-image startup starts empty as intended.
- [ ] RDB/manifest/WAL corruption fails startup without overwriting the image.
- [ ] SIGTERM exits 0 and publishes a checkpoint.
- [ ] `/health` or PING succeeds and `/stats` or STATS is coherent.
- [ ] HTTP and KV CRUD smoke tests pass.
- [ ] Restart recovery is verified after a kill and after a graceful stop.
- [ ] Backup and restore of the complete persistence set is tested.
- [ ] Security boundary prevents untrusted network access.
- [ ] Local CI/acceptance evidence is archived with the release.
