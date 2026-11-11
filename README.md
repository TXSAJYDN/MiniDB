# MiniDB

MiniDB 是一个使用 C++20 实现的**面向有界资源的单机 crash-consistent KV 存储服务**。

> A crash-consistent single-node KV store for bounded-resource environments.

它通过两个独立前端访问同一类存储引擎：

- 严格但范围有限的 HTTP/1.1 子集；
- 二进制安全的 TCP KV 协议，支持 CRUD、TTL、SAVE 和 STATS。

项目当前的核心问题不是“再实现一个 Redis”，而是验证一条完整持久化闭环：

1. 客户端收到写成功响应后，进程在任意点崩溃，数据是否一定还能恢复？
2. `SYNC` 与 `INTERVAL` 分别允许什么丢失窗口，代价是多少？
3. 恢复过程如何区分 WAL 尾部半写、中间损坏、manifest 损坏和快照缺失？

MiniDB 是可靠性工程原型，**不宣称生产可用**，不宣称 Redis/SQL 兼容，也不宣称高并发或生产 SLA。

## 核心机制

- 写入先追加 WAL，再按 durability 级别决定何时返回成功；
- RDB 是全量 checkpoint，不再承担每一次写入的持久化承诺；
- manifest 原子记录 `checkpoint_lsn` 和必须保留的 WAL segment；
- 启动时先构建临时恢复镜像，RDB、manifest 和 WAL 全部验证通过后才发布 Store；
- WAL 尾部半写可截断到最后合法记录，中间损坏、LSN 断层或缺失 segment 拒绝启动；
- TTL 在 WAL 中保存绝对 Unix 毫秒 deadline，重启不会重新延长 TTL；
- `<rdb_path>.lock` 使用非阻塞 `flock`，阻止两个服务进程同时写同一数据目录。

## Durability 语义

| 模式 | 成功响应条件 | 已知确认写保证 | 可能丢失 |
|---|---|---|---|
| `SYNC`（默认） | WAL append + 文件 fsync 成功，再应用内存并响应 | 在 fsync/设备模型成立时，确认写可跨进程崩溃恢复 | 对已确认写不主动允许丢失 |
| `INTERVAL` | WAL append 成功并应用内存后响应 | 依赖周期 flush；checkpoint/正常关闭前强制 flush | 最近一个 flush 窗口内的确认写，约 100 ms 量级，受调度和 I/O 影响 |

本项目只实现 `SYNC` 与 `INTERVAL`。`BATCHED` 未实现，属于未启动的 R4 可选项。

这里的“crash-consistent”针对已记录的进程崩溃与重启语义；测试使用故障注入和 `SIGKILL`，**不证明掉电、内核崩溃、设备缓存撒谎或介质损坏下的持久性**。

## 快速开始

MiniDB 依赖 Linux 的 epoll、eventfd、timerfd、signalfd 和 POSIX 文件系统语义，不能在 Windows 上使用 MSVC 原生构建。开发和验收应在 WSL2 Ubuntu 24.04 或 Linux 原生文件系统中进行。

~~~bash
sudo apt update
sudo apt install -y build-essential cmake git python3 curl clang clang-tidy
cmake --preset release
cmake --build --preset release -j8
ctest --preset release --output-on-failure
~~~

### 启动 HTTP 服务

~~~bash
./build/release/app/minidb_http_server \
  -p 8080 -r /tmp/minidb-http/minidb.rdb -d sync

curl -i http://127.0.0.1:8080/health
curl -i -X POST --data-binary 'hello' http://127.0.0.1:8080/kv/greeting
curl -i http://127.0.0.1:8080/kv/greeting
curl -i http://127.0.0.1:8080/stats
curl -i -X POST http://127.0.0.1:8080/save
~~~

### 启动 KV 服务

~~~bash
./build/release/app/minidb_kv_server \
  -p 6379 -r /tmp/minidb-kv/minidb.rdb -d interval

python3 test_kv_client.py 6379
~~~

HTTP 和 KV 服务不应同时指向同一个 RDB 路径。单写者锁会拒绝第二个进程，但不要让两个服务共享一个逻辑数据集。

收到 `SIGINT` 或 `SIGTERM` 后，服务会停止接收请求、flush INTERVAL WAL，并执行最终 checkpoint。正常关闭退出码为 0 才表示最终 checkpoint 成功。

## 持久化布局

给定 `-r /var/lib/minidb/minidb.rdb`：

~~~text
/var/lib/minidb/minidb.rdb
/var/lib/minidb/minidb.rdb.manifest
/var/lib/minidb/minidb.rdb.wal/
/var/lib/minidb/minidb.rdb.lock
~~~

WAL segment 名：

~~~text
wal-00000000000000000001.log
~~~

只复制 RDB 文件不能保证获得最新状态；完整备份必须包含 RDB、manifest 和 WAL 目录，或先停止进程再复制完整集合。详见 [部署与运维](docs/deployment.md)。

## 资源边界

| 项目 | 上限 |
|---|---:|
| 服务进程并发连接 | 32 |
| 每连接输入/输出预算 | 各 1.25 MiB |
| 单个 key | 1 KiB |
| 单个 value / WAL 记录 | 1 MiB |
| WAL segment | 16 MiB |
| WAL 总字节 | 256 MiB |
| 活跃 WAL segment 数 | 128 |
| 恢复扫描记录 | 1,000,000 |
| 恢复字节 | 512 MiB |
| RDB 快照 | 64 MiB / 100,000 records |

WAL 达到 segment 数或字节预算时，写路径会先触发同步 checkpoint；checkpoint 失败则拒绝写，不应无限增长。具体策略见 [ADR-004](docs/decisions/004-wal-resource-policy.md)。

## 恢复与失败行为

启动顺序：

1. 获取 `<rdb_path>.lock`；
2. 读取并校验 manifest；
3. 加载 RDB 到临时 Store；
4. 按 LSN 扫描并校验 manifest 引用的 WAL segment；
5. 只重放 `LSN > checkpoint_lsn` 的记录；
6. 仅在尾部不完整时截断到最后合法前缀并 fsync；
7. 完整成功后发布 Store 并开始监听。

以下情况会非零退出且不监听：

- manifest CRC、版本或 segment 列表非法；
- manifest 引用的 RDB 或 WAL segment 缺失；
- WAL 中间损坏、重复 LSN、LSN 不连续或 segment 范围不一致；
- 恢复资源超过记录数/字节上限；
- 另一个进程持有写者锁。

恢复失败不会修改损坏现场，也不会发布部分 Store。只有 WAL 尾部半写是允许截断的情形。

## 可观测性

KV 的 `STATS`（命令类型 8）和 HTTP 的 `GET /stats` 返回同一稳定 JSON 结构，包含：

- durability mode、last/next LSN、当前 WAL 字节数和 segment 数；
- mutation、fsync、checkpoint、interval flush 计数；
- checkpoint 暂停时间；
- WAL append、rotation、segment 清理和目录 fsync 计数；
- 成功 append、data fsync 与 `SYNC` 持久化路径的累计/最大耗时，以及固定 fsync 延迟桶；
- 启动恢复的 RDB 记录数、扫描/重放记录数、截断字节和遗留 residue；
- legacy import 与 bootstrap 状态。

字段定义见 [KV 协议](docs/protocol-kv.md) 和 [HTTP 协议](docs/protocol-http.md)。
`timing` 指标只记录成功操作；`max` 是进程生命周期最大值，包含 preload/warm-up，
不是本轮窗口最大值，也不是生产 SLO。固定桶不提供任意分位数或逐请求 trace。

## 验证状态

当前 R0-R3 已完成实现与本地验收；R4-A 已完成 ADR 和 baseline instrumentation，
组提交实现尚未开始：

- 六种构建配置的真实 CTest 矩阵；
- WAL 编解码、manifest、恢复、checkpoint、单写者锁和故障注入单元测试；
- 19 个进程级用例，覆盖 HTTP/KV 持久化、尾部截断、中间损坏、缺失 segment、manifest 损坏、随机 kill、TTL 和 STATS；
- HTTP、KV、WAL record、manifest 四个固定 seed fuzz target；
- 多 durability、多客户端、写/混合/TTL 的 5 轮可靠性 benchmark；
- R4-A timing 字段、固定 fsync 桶和 v2 benchmark 采集契约；
- clang-format、clang-tidy 和 `git diff --check`。

具体数字、限制和证据见 [可靠性证据](docs/reliability-evidence.md)。R0-R3 基线
`cfbda60` 的远程 GitHub Actions 已 9/9 通过；R4-A 提交的最终状态以推送后的
GitHub checks 为准。本项目的 R4-A 只做证据采集，不实现组提交。

## 明确的非目标

- 不提供 SQL；
- 不追求完整 Redis 命令/协议兼容；
- 不提供复制、Raft、集群或分片；
- 不提供事务、MVCC 或隔离级别；
- 不提供 TLS、认证或授权；
- 不承诺生产 SLA、容量规划或高并发表现；
- 不覆盖多进程共享数据目录、掉电一致性或设备故障证明；
- 不实现 `BATCHED`、组提交或后台 checkpoint；R4-A 只完成
  [ADR-005](docs/decisions/005-group-commit.md) 和指标采集，组提交实现仍为
  NOT STARTED。

## 文档

- [可靠性与持久化升级计划书](docs/MiniDB-可靠性与持久化升级计划书.md)
- [项目状态](docs/PROJECT_STATE.md)
- [系统架构](docs/architecture.md)
- [WAL 与 manifest 格式](docs/wal-format.md)
- [恢复模型](docs/recovery.md)
- [崩溃一致性](docs/crash-consistency.md)
- [可靠性证据](docs/reliability-evidence.md)
- [部署与运维](docs/deployment.md)
- [HTTP 协议](docs/protocol-http.md)
- [KV 协议](docs/protocol-kv.md)
- [RDB v2 格式](docs/rdb-format.md)
- [可复现 benchmark](bench/README.md)
- [Fuzz targets](fuzz/README.md)
- [ADR-005：R4-A 组提交候选设计与证据门槛](docs/decisions/005-group-commit.md)
- [架构决策记录](docs/decisions/)
