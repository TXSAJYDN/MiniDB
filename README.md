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

- owner EventLoop 负责校验、排序、内存 apply 和协议响应，专用 writer thread 独占 WAL append/fsync；
- 连续 WAL 记录由 writer 合并为有界 batch，一次 data fsync 覆盖整批；`SYNC` 的响应仍严格晚于覆盖该 LSN 的 fsync；
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

本项目只实现 `SYNC` 与 `INTERVAL`。组提交是 `SYNC` 的内部批处理优化，不是新的公开 durability mode；不存在 `BATCHED` 选项。

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
| group-commit admission queue | 4,096 entries / 64 MiB |
| 单个物理 WAL batch | 256 records / 16 MiB |
| group-commit completion backlog | 4,096 entries / 64 MiB |

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
- 顶层 `group_commit` 对象：queue/backlog 高水位、拒绝数、batch 数量与大小分布、data fsync 数、writer 各阶段耗时、失败计数和 writer 状态；
- 启动恢复的 RDB 记录数、扫描/重放记录数、截断字节和遗留 residue；
- legacy import 与 bootstrap 状态。

字段定义见 [KV 协议](docs/protocol-kv.md) 和 [HTTP 协议](docs/protocol-http.md)。
`timing` 指标只记录成功操作；`max` 是进程生命周期最大值，包含 preload/warm-up，
不是本轮窗口最大值，也不是生产 SLO。固定桶不提供任意分位数或逐请求 trace。

## 验证状态

R4-A 组提交实现已完成并通过本地质量门禁；正式 G4 收益证据在推送后的 clean HEAD 上采集：

- Debug、Release、ASan、UBSan、ASan+UBSan、TSan 六种构建配置；
- 每个配置 **338 项 CTest**，包含 24 个 `ProcessIntegrationTest.*` 进程级用例；
- 组提交 writer 的顺序、batch 上限、queue/backlog full、append/fsync 失败、cleanup barrier、fail-stop 和 callback 恰好一次测试；
- HTTP/KV 异步 mutation、断开连接安全、流水线响应顺序、SAVE/STATS 和 shutdown 失败退出码测试；
- HTTP、KV、WAL record、manifest 四个固定 seed fuzz target；
- clang-format、clang-tidy 和 `git diff --check`；
- 多 durability、多客户端、写/混合/TTL 的 5 轮可靠性 benchmark；
- R4-A v2 baseline：18 配置 × 5 轮、414,688 次成功请求、0 错误；32 客户端
  `SYNC` write 的 fsync 占持久化耗时中位数 99.06%，G1/G2 PASS；
- R4-A G3：独立 batch-formation 实验 5 轮、2,985 个 WAL data fsync 窗口、
  0 错误，汇总 `pending_count >= 8` 比例 **0.987605**，PASS；
- 正式 G4 的 clean-HEAD 结果、原始 artifact 摘要和限制见
  [可靠性证据](docs/reliability-evidence.md)。

具体数字、限制和证据见 [可靠性证据](docs/reliability-evidence.md)。远程 CI 必须在
最终 `main` HEAD 上通过；本地门禁不替代 GitHub Actions。G3 的 pending 计数
是批处理机会上界，不是服务器队列已有记录的证据。

## 明确的非目标

- 不提供 SQL；
- 不追求完整 Redis 命令/协议兼容；
- 不提供复制、Raft、集群或分片；
- 不提供事务、MVCC 或隔离级别；
- 不提供 TLS、认证或授权；
- 不承诺生产 SLA、容量规划或高并发表现；
- 不覆盖多进程共享数据目录、掉电一致性或设备故障证明；
- 不提供公开 `BATCHED` durability mode；组提交只作为 `SYNC` 的内部优化存在；
- R4-A 不包含后台/增量 checkpoint、事务、MVCC 或多写者。

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
- [ADR-006：R4-A 组提交实现](docs/decisions/006-group-commit-implementation.md)
- [架构决策记录](docs/decisions/)
