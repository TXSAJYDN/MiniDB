# Deployment and operation

## Status and supported environment

MiniDB is an educational single-node database, not a production service. The
verified environment is Ubuntu 24.04 under WSL2 with GCC 13 or Clang 18. Other
Linux distributions may work, but should be revalidated, especially TSan and
performance measurements.

The authoritative source tree is the Linux ext4 worktree used for acceptance.
A Windows 9p copy is not authoritative for builds, tests, or benchmark results.

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
cmake -S . -B build/release   -DCMAKE_BUILD_TYPE=Release   -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/release -j2
ctest --test-dir build/release --output-on-failure
~~~

The resulting executables are:

~~~text
build/release/app/minidb_http_server
build/release/app/minidb_kv_server
~~~

For a reproducible six-configuration acceptance build, use
tools/run_acceptance_matrix.py as documented in tools/README.md. Never reuse a
previous acceptance root for a new run.

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
access to the RDB file's directory because saving and directory fsync require it.
It also needs read access to an existing RDB file and execute permission on the
parent directories.

The two servers must use different RDB paths. MiniDB does not implement an
inter-process file lock.

## Command line

Both programs share the same option parser:

~~~text
-p <port>      listening port
-r <path>      RDB file path
-h             help; must be used alone
~~~

Defaults:

| Program | Port | RDB path |
|---|---:|---|
| minidb_http_server | 8080 | ./minidb.rdb |
| minidb_kv_server | 6379 | ./minidb.rdb |

Duplicate options, unknown options, positional arguments, empty values,
non-canonical decimal ports, and ports outside 1..65535 are rejected with exit
code 2. Use an absolute RDB path in a service unit so the working directory
cannot change startup behavior.

Examples:

~~~bash
/opt/minidb/bin/minidb_http_server   -p 8080 -r /var/lib/minidb/http/minidb.rdb

/opt/minidb/bin/minidb_kv_server   -p 6379 -r /var/lib/minidb/kv/minidb.rdb
~~~

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
ExecStart=/opt/minidb/bin/minidb_http_server -p 8080 -r /var/lib/minidb/http/minidb.rdb
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
ExecStart=/opt/minidb/bin/minidb_kv_server -p 6379 -r /var/lib/minidb/kv/minidb.rdb
~~~

SIGTERM starts graceful shutdown and final snapshot persistence. A nonzero exit
from the save path marks the unit failed. Do not use SIGKILL as the normal stop
mechanism; it bypasses the final save.

## Startup validation

Before accepting clients, the process:

1. validates its command line;
2. blocks SIGINT/SIGTERM on the owner thread;
3. binds the listening socket;
4. loads the RDB path or starts empty if and only if the file is absent;
5. begins listening.

A missing RDB is not an error. A file with bad checksum, version, varint,
record length, duplicate key, excess size, or invalid path type is an error. The
process exits nonzero and must not be allowed to overwrite the damaged file
automatically.

Check startup logs and exit status:

~~~bash
/opt/minidb/bin/minidb_http_server -p 18080 -r /tmp/minidb-smoke.rdb
curl -fsS http://127.0.0.1:18080/health
~~~

## Health and smoke checks

HTTP:

~~~bash
curl -i http://127.0.0.1:8080/health
curl -i -X POST --data-binary 'value' http://127.0.0.1:8080/kv/example
curl -i http://127.0.0.1:8080/kv/example
curl -i -X DELETE http://127.0.0.1:8080/kv/example
curl -i -X POST http://127.0.0.1:8080/save
~~~

KV:

~~~bash
python3 test_kv_client.py 6379
~~~

A successful /save or SAVE response means the temp file was written, fsynced,
renamed, and the parent directory was fsynced. A directory-fsync failure after
rename is reported as RDB durability unconfirmed.

## Backup

RDB is replaced by an atomic rename. A safe application-consistent backup can
copy the last successfully saved file while the process is quiescent or after a
successful explicit SAVE.

~~~bash
curl -fsS -X POST http://127.0.0.1:8080/save
cp --reflink=auto /var/lib/minidb/http/minidb.rdb   /var/backups/minidb/http-$(date +%Y%m%dT%H%M%S).rdb
~~~

For the KV server, issue SAVE with a binary client or stop the service cleanly.
Do not copy a temporary .tmp file and do not modify or truncate the live file.

## Restore

1. Stop the owning server.
2. Preserve the current RDB for diagnosis.
3. Put a known-good v2 RDB at the configured path with correct ownership and
   permissions.
4. Start the server in the foreground and confirm it loads without error.
5. Run HTTP health or KV PING and a read smoke test.

~~~bash
systemctl stop minidb-http
cp -p /var/backups/minidb/http-<timestamp>.rdb   /var/lib/minidb/http/minidb.rdb
chown minidb:minidb /var/lib/minidb/http/minidb.rdb
systemctl start minidb-http
~~~

If the file is corrupt, startup must fail rather than silently starting empty.
Do not delete the only copy to force startup.

## Capacity and resource planning

Hard limits are documented in the protocol and RDB references. Operationally:

- at most 32 concurrent TCP connections per process;
- 1.25 MiB input and output budgets per connection;
- 1 MiB maximum values;
- 100,000 RDB records;
- 64 MiB maximum RDB file;
- owner-loop processing, so a synchronous SAVE blocks new request handling.

Set an appropriate service file-descriptor limit. Keep the RDB directory on a
filesystem with reliable fsync and rename semantics. WSL2 is appropriate for
development and benchmark-limitation evidence, not for a production durability
claim.

## Security

MiniDB has no TLS, authentication, or authorization. Bind it only to a trusted
interface or protect it with an external network boundary. Anyone who can reach
the HTTP or KV port can read and modify all keys and can request a synchronous
save.

Do not expose the KV port to the public internet. Do not run the server as root.
Restrict the RDB directory to the service account because it contains all data.

## Upgrade and rollback

Version 2 is the only accepted RDB version. Before replacing a binary:

1. run the target build's full local tests;
2. make a backup of the current snapshot;
3. stop the old process gracefully;
4. install the new binary;
5. start it against a copy of production data in a staging directory first;
6. verify startup and read/write behavior;
7. retain the previous binary and snapshot for rollback.

There is no online migration or rolling upgrade path.

## Shutdown behavior

SIGINT and SIGTERM are delivered through signalfd to the owner loop. After the
loop exits, the server performs a final save. Exit code 0 requires Ok from the
snapshot path. A Failed or DurabilityUnconfirmed result returns nonzero.

The correct unit stop sequence is therefore:

~~~bash
systemctl stop minidb-http
systemctl status minidb-http
~~~

Do not assume that a visible new RDB file alone proves crash durability. Follow
the process exit code and the log message.

## Deployment acceptance checklist

- [ ] Release binary built from the intended source digest.
- [ ] Configuration uses absolute paths and distinct RDB files.
- [ ] Service runs as an unprivileged account.
- [ ] Parent directory is writable and fsync-capable.
- [ ] Missing-file startup starts empty as intended.
- [ ] Corrupt-file startup fails without overwriting the file.
- [ ] SIGTERM exits 0 and writes the RDB.
- [ ] /health or PING succeeds.
- [ ] HTTP and KV CRUD smoke tests pass.
- [ ] Restart persistence is verified.
- [ ] Backup and restore procedure is tested.
- [ ] Security boundary prevents untrusted network access.
- [ ] Local CI/acceptance evidence is archived with the release.
