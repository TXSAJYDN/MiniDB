# MiniDB 项目状态

- 更新时间：2026-11-11（Asia/Shanghai）
- 当前定位：**面向有界资源的单机 crash-consistent KV 存储服务**
- 权威开发工作树：`/home/win11/projects/miniDB`（WSL2 Ubuntu 24.04）
- Windows 同步工作树：`E:\c++\miniDB`
- 远程仓库：`git@github.com:TXSAJYDN/MiniDB.git`
- 当前阶段：**R0-R3 已完成；R4-A 组提交实现与 G5/G6 本地验证完成，G4 收益证据待 clean HEAD 采集**
- 当前测试清单：**338 项 CTest**（以最终六配置本地验证矩阵为准）

## 1. 当前结论

MiniDB 已从单一 mini KV + RDB 快照升级为一条可以被测试和证伪的持久化闭环：

> 客户端收到写成功响应后，进程在任意点崩溃，已确认写是否还能恢复？

当前实现提供：

- 逻辑 WAL、CRC32C、严格递增 LSN 和 segment 轮转；
- `SYNC` 与 `INTERVAL` 两种 durability 模式，默认 `SYNC`；
- RDB v2 作为全量 checkpoint，不再承担每一笔写入的持久化承诺；
- checkpoint manifest 原子记录 `checkpoint_lsn` 与保留的 WAL segment；
- 启动时 RDB + manifest + WAL 全量校验后一次性发布临时 Store；
- 仅允许最后一个 WAL segment 的尾部半写截断，中间损坏拒绝启动；
- `<rdb_path>.lock` 上的非阻塞 `flock` 单写者锁；
- owner EventLoop 唯一 admission/apply/ack 点与专用 WAL writer thread；
- 连续 LSN 合并为有界 batch，一次 data fsync 覆盖整批，不新增公开 durability mode；
- 有界 admission queue、物理 batch 和 completion backlog，满时明确拒绝或 fail-stop；
- SAVE、checkpoint 和 shutdown 通过 writer barrier 排空并只在目标 LSN apply 后发布 manifest；
- HTTP `/stats` 与 KV `STATS` 共享的持久化诊断快照；
- R4-A 成功 append、data fsync、`SYNC` 持久化路径计时、固定 fsync 延迟桶和顶层 `group_commit` 指标；
- C01-C14 崩溃点、固定 seed 随机 `kill -9`、尾部/中间损坏和故障注入测试；
- HTTP、KV、WAL record、checkpoint manifest 四个 fuzz target；
- 18 种配置的 durability/客户端/负载可靠性 benchmark。

当前不宣称生产级、高并发、Redis/SQL 兼容或生产 SLA。这里的
“crash-consistent”只覆盖已测试的进程崩溃与重启语义，不证明掉电、内核崩溃、
设备缓存撒谎或介质损坏下的持久性。

## 2. 阶段状态

| 阶段 | 状态 | 当前依据 |
|---|---|---|
| A-H 功能基线 | DONE（历史） | 214 项测试、六配置、旧 CI 9/9；作为历史基线保留 |
| R0 重新定位与文档基线 | DONE | README、架构、部署、ADR、证据规范和“声明-证据-限制”口径 |
| R1 WAL + RDB checkpoint 正确性基线 | DONE | WAL/manifest 编解码、轮转、恢复、checkpoint、legacy import、单写者锁 |
| R2 崩溃一致性与恢复模型闭环 | DONE | C01-C14、随机 kill、故障注入、四个 fuzz target、进程级崩溃/恢复覆盖 |
| R3 性能、恢复与公开证据 | DONE | 18 配置 × 5 轮 benchmark、恢复/暂停/RSS/写放大数据 |
| R4-A 组提交 | IMPLEMENTED; G1-G3 PASS; G4 CLEAN-HEAD EVIDENCE PENDING; G5/G6 LOCAL PASS | ADR-006、writer/queue/batch/completion、异步协议、SAVE/shutdown barrier、338 项测试与完整质量门禁 |
| R4-B 后台/增量 checkpoint | NOT STARTED | 需要独立证据和原子发布语义回归 |
| R4-C MVCC/事务 | NOT STARTED | 独立阶段，不与 WAL 基线混合 |

## 3. 新定位与范围

> **MiniDB is a crash-consistent single-node KV store for bounded-resource
> environments.**

核心问题：

1. `SYNC` 下，客户端收到成功响应后进程崩溃，数据是否一定还能恢复？
2. `INTERVAL` 允许什么丢失窗口，代价是什么？
3. 恢复如何区分 WAL 尾部半写、中间损坏、manifest 损坏和快照缺失？
4. 在连接数、记录大小、WAL 大小和恢复时间都有硬上限时，失败行为是否可解释？

明确非目标：

- SQL 解析器/执行器；
- 完整 Redis 命令和协议兼容；
- 复制、Raft、集群、分片；
- 事务、MVCC、隔离级别；
- TLS、认证、授权、多租户；
- 多进程共享一个数据目录；
- 生产 SLA、容量规划或高并发宣传；
- 公开 `BATCHED` durability mode；组提交仅作为 `SYNC` 的内部优化；
- 后台/增量 checkpoint、事务、MVCC 或多写者。

## 4. 持久化语义

### 4.1 布局

给定 `-r <rdb_path>`：

~~~text
<rdb_path>              RDB v2 checkpoint snapshot
<rdb_path>.manifest     checkpoint manifest v1
<rdb_path>.wal/         WAL segment 目录
<rdb_path>.lock         flock 单写者锁
~~~

### 4.2 写路径

- owner EventLoop 为每条 mutation 分配严格连续 LSN，并提交给专用 writer；
- writer 将连续 LSN 合并为最多 256 条或编码后 16 MiB 的物理 batch，执行一次尾部 data fsync；
- `SYNC`：覆盖该 LSN 的 batch fsync 成功后，completion 回到 owner，才 apply 并返回成功；
- `INTERVAL`：append 后即可 apply 并返回；100 ms timer、checkpoint 和正常关闭前通过 writer barrier 强制 flush；
- WAL fsync/append 失败后持久化层 fail-stop；LSN、字节计数和统计不回滚；
- fsync 失败时完整记录可能已落盘，恢复允许出现未确认写；
- 不允许已确认的 `SYNC` 写在恢复后消失。

### 4.3 checkpoint 顺序

1. 暂停新 mutation admission，并向 writer 发出 pause barrier；
2. writer 排空此前已接受的连续记录，completion 回到 owner 按 LSN apply；
3. 暂停 TTL scanner，flush 并 fsync WAL；
4. 必要时轮转 active segment；
5. 发布 RDB 快照；
6. 原子发布 manifest；
7. 清理旧 WAL 并 fsync WAL 目录；
8. 成功后 resume，或进入 shutdown 的最终停止阶段。

任何快照或 manifest 的持久性不确定都会 fail-stop，并保留旧 WAL。

### 4.4 恢复

1. 获取单写者锁；
2. 读取并校验 manifest；
3. 加载 RDB 到临时 Store；
4. 按 LSN 扫描 manifest 引用的 segment 及其后所有 segment；
5. 只重放 `LSN > checkpoint_lsn` 的记录；
6. 仅截断最后一个 segment 的尾部半写并 fsync；
7. 全部成功后发布 Store 并开始监听。

恢复失败：非零退出、不监听、不修改损坏现场、不发布部分 Store。

## 5. 资源上限

| 项目 | 上限 |
|---|---:|
| 服务进程并发连接 | 32 |
| 每连接输入/输出预算 | 各 1.25 MiB |
| 单个 key | 1 KiB |
| 单个 value / WAL 记录 | 1 MiB |
| WAL segment | 16 MiB |
| WAL 总字节 | 256 MiB |
| WAL segment 数 | 128 |
| 恢复扫描记录 | 1,000,000 |
| 恢复字节 | 512 MiB |
| RDB 快照 | 64 MiB / 100,000 records |
| group-commit admission queue | 4,096 entries / 64 MiB |
| 单个物理 WAL batch | 256 records / 16 MiB |
| group-commit completion backlog | 4,096 entries / 64 MiB |

## 6. 当前验证状态

R4-A 组提交实现与本地质量门禁包括：

- Debug、Release、ASan、UBSan、ASan+UBSan、TSan 六种配置；
- 每个配置 **338 项 CTest**；
- **24 个 `ProcessIntegrationTest.*` 进程级用例**，含 HTTP/KV 异步协议、客户端断开、shutdown 失败退出码；
- HTTP、KV、WAL record、checkpoint manifest 四个 fuzz target，各 20,000 runs，seed `20261111`；
- clang-format dry-run、clang-tidy、`git diff --check`；
- 18 配置 × 5 轮 R3 可靠性 benchmark，seed `20261111`；
- R4-A 的 `/stats`/`STATS` timing 契约、fsync 桶分区和 v2 可靠性 benchmark；
- v2 baseline：18 配置 × 5 轮、414,688 次成功请求、0 错误；G1/G2 PASS；
- G3：独立非生产 batch formation 实验 5 轮、2,985 次成功 `SET`、2,985 个
  WAL data fsync 窗口、0 错误；每轮 `pending_count` P50/P90 均为 32，汇总
  `>=8` 比例 **0.987605**，判定 PASS；
- 远程 CI：R0-R3 基线 `cfbda60` 的 GitHub Actions 9/9 通过；R4-A 最终 HEAD
  需要重新跑完整 CI，以该 commit 的 GitHub checks 为准；
- 本地证据不替代远程 CI，也不构成生产验收。

正式 benchmark 原始数据：

~~~text
R3: bench/results/reliability-20261111.json
bytes: 133,299
sha256: a5c3099789be938654771f830acbd25ecedc1da5520f7c4aa1567493c0e74254

R4-A v2: bench/results/reliability-20261111-r4a-baseline.json
bytes: 310,067
sha256: 55bcad4ead05fcba26e44cecf17e71df6ffd7dba17e54a128398c5b65820fe84
git_head: 964ba8446a71f6c491cd1241e5479632465b9835
working_tree_dirty_at_collection: false

R4-A G3: bench/results/r4a-batch-formation-20261111.json
bytes: 1,435,200
sha256: f68b485204600ba83d88dd04834ee2df08595776af150cdfb164846ae6b4439d
git_head: 5d26f0be1dba0ff7e383d9660999c50204aa831f
working_tree_dirty_at_collection: false
harness_sha256: 8eafe2b777de7da6413086a18e84d920375c9ea227132c9df96a98d15b77f629
binary_sha256: 5eb7f8055489bc9bfb792a219bdc7c685760fa4020fe16ff7c15de53c2f60d38
~~~

以下为历史 R3 artifact 的汇总；R3 数据采集自未提交工作树，环境为 WSL2 同机
loopback closed-loop，不是容量或生产 SLA 证据。汇总：

| 指标 | SYNC | INTERVAL |
|---|---:|---:|
| median QPS 范围 | 408.815 - 2,219.464 | 2,020.629 - 3,367.577 |
| recovery median | 0.022716 - 0.023829 s | 0.043730 - 0.086119 s |
| checkpoint pause median | 11,571 - 20,852 us | 12,387 - 19,323 us |
| server RSS peak max | 4,608 KiB | 4,608 KiB |

R3 的 18 个配置共 340,240 次成功请求、0 错误，采集总耗时 238.703 秒；R4-A v2
baseline 的对应数字见上文。

## 7. 已知限制

- durability 结论只覆盖进程崩溃；未证明掉电、内核崩溃、设备缓存或介质故障。
- checkpoint 是 stop-the-world，会暂停 owner loop；未实现后台/增量 checkpoint。
- `INTERVAL` 最近一个 flush 窗口内的已确认写可能丢失。
- 只有单机单写者；`flock` 是 advisory lock，不能阻止第三方直接修改文件。
- 32 连接上限保持不变；benchmark 的 32-client 配置是边界，不是扩展性结论。
- 无 TLS、认证、授权、复制、集群、事务或 MVCC。
- WSL2 结果与硬件计数器限制必须随数据一起说明。
- G3 的 `pending_count` 是外部请求重叠机会的上界，不是 G4 收益证明。
- group commit 的收益、P50/P99、CPU/RSS、写放大和恢复时间以 clean HEAD 的
  正式 G4 artifact 为准；未采集前不得把设计目标当成实测结果。
- 许可证仍由所有者决定；仓库未创建 `LICENSE`/`COPYING`。

## 8. 文档入口

- [README](../README.md)
- [可靠性与持久化升级计划书](MiniDB-可靠性与持久化升级计划书.md)
- [系统架构](architecture.md)
- [WAL 与 manifest 格式](wal-format.md)
- [恢复模型](recovery.md)
- [崩溃一致性](crash-consistency.md)
- [可靠性证据](reliability-evidence.md)
- [部署与运维](deployment.md)
- [HTTP 协议](protocol-http.md)
- [KV 协议](protocol-kv.md)
- [RDB v2 格式](rdb-format.md)
- [故障排查](troubleshooting.md)
- [Benchmark](../bench/README.md)
- [Fuzz targets](../fuzz/README.md)
- [阶段 A-H 历史验收报告](A_H_ACCEPTANCE_REPORT.md)
- [ADR-005：R4-A 组提交候选设计与证据门槛](decisions/005-group-commit.md)

## 9. 后续工作

1. 提交 R4-A 实现并在 `main` 上跑完整 GitHub CI；
2. CI 全绿后，在 clean HEAD 上采集正式 G4（5 轮，32 客户端 write），验证
   median QPS `>= 836` 且不低于 baseline 418.018 的 2 倍；
3. 将 G4 artifact、P50/P99、CPU、RSS、写放大和恢复时间写入
   `docs/reliability-evidence.md`，更新 ADR-006 的最终状态；
4. 提交并推送 G4 证据，确保最终 `main` CI 再次全绿；
5. 后台/增量 checkpoint 与 MVCC/事务保持独立阶段，不与 R4-A 混合。

任何 R4 工作都必须重新通过 TSan、崩溃矩阵和资源上限验证，不能牺牲
“确认写不丢、恢复失败不发布部分 Store”的语义。
