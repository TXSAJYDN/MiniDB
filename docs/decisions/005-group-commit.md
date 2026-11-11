# ADR-005：R4-A 组提交候选设计与证据门槛

- 状态：Proposed — evidence-gated，未实现
- 日期：2026-11-11
- 关联：`docs/MiniDB-可靠性与持久化升级计划书.md`、`docs/architecture.md`、`docs/reliability-evidence.md`
- 决策范围：R4-A 的设计、观测口径和准入条件；不包含组提交实现

## 背景

当前 `SYNC` 写路径在 owner EventLoop 上串行执行：

1. 生成 WAL record 并分配严格连续的 LSN；
2. `WalManager::append` 写 WAL 文件；
3. 对 WAL 文件执行一次 data `fsync`；
4. 应用内存状态；
5. 返回成功响应。

因此，一个成功的 `SYNC` mutation 至少对应一次 WAL data fsync；segment 轮转、
首次 bootstrap 和 checkpoint 还可能增加 fsync 次数。网络解析、持久化、内存
apply 和响应序列化共享同一条 owner-loop 执行路径。

R3 的 32 客户端 write 基准中，`SYNC` median QPS 为 418.018，`INTERVAL` 为
3,000.253；但两者持久化语义不同，且旧版指标没有拆分 append、fsync 和
持久化端到端耗时。这个差距只能说明值得调查，不能单独证明组提交会带来收益。

组提交会引入专用 writer、跨线程 completion、有界队列、stop/drain 和新的崩溃
窗口。没有服务器端阶段数据就实现它，会把一个可验证的持久化原型变成难以证明的
并发系统。因此 R4-A 先建立证据门槛。

## 决策

### R4-A.0：只增加诊断指标，不改变写路径

本阶段只增加固定内存、低开销的累计计时和固定桶分布，不增加 writer thread、
队列、batch、异步 completion 或新的 durability mode。`SYNC` 和 `INTERVAL`
的 ack 时机、WAL 格式、LSN 顺序、恢复语义和 32 连接上限均保持不变。

指标通过 `GET /stats` 和 KV `STATS` 以新增的顶层 `timing` 对象暴露。它们用于
判断是否值得进入实现阶段，不是生产 SLO。

### 指标口径

| 字段 | 语义 |
|---|---|
| `wal_append_total_us` | 成功 WAL append 的累计耗时；只覆盖 `write_all()`，不含编码、轮转和 fsync |
| `wal_append_max_us` | 上述操作在进程生命周期内的最大值 |
| `wal_data_fsync_total_us` | 成功的 WAL data fsync 累计耗时；包含空 segment 创建和轮转产生的 data fsync，不含目录 fsync |
| `wal_data_fsync_max_us` | 上述 fsync 的进程生命周期最大值 |
| `sync_mutation_total_us` | 成功 `SYNC` 从 mutation 校验后进入持久化到内存 apply 的累计耗时；不含网络解析、参数校验和响应序列化；包含首次 bootstrap 或 checkpoint retry 触发的持久化工作 |
| `sync_mutation_max_us` | 上述端到端持久化路径的进程生命周期最大值 |
| `fsync_latency_bucket_bounds_us` | 成功 data fsync 的固定上界，最后还有一个大于最大上界的 overflow 桶 |
| `fsync_latency_bucket_counts` | 各桶累计计数，总和必须等于 `wal.data_fsyncs` |

固定上界为：

~~~text
50, 100, 250, 500, 1000, 2500, 5000,
10000, 25000, 50000, 100000 us
+ overflow
~~~

计数器只记录成功操作；失败 append 或失败 fsync 不增加对应计数。`total` 用于
计算测量窗口均值，`max` 只表示进程生命周期最大值，包含 preload 和 warm-up，
不能直接当作某一轮窗口的最大值。固定桶限制内存和 cardinality，不提供任意
分位数或逐请求 trace。

### 候选实现（仅在准入门槛满足后）

组提交候选采用以下边界；具体 API、线程模型和 batch 大小必须另立实现 ADR 或
在本 ADR 状态变更时补充：

- 专用 WAL writer thread，owner EventLoop 不再直接等待 WAL fsync；
- owner 侧仍是唯一的 admission 点，保持全局 LSN 顺序和单写者语义；
- 有界队列，队列条目包含 record、LSN、completion 和必要的计时信息；
- writer 只消费连续的队列前缀，将多个连续 LSN 写入 WAL 后执行一次 data
  fsync；
- completion 回到 owner EventLoop；只有覆盖该 LSN 的 fsync 成功，才允许应用
  内存状态并返回成功响应；
- 队列满时通过背压或明确的 resource-limit 拒绝新的提交，不能无界增长、静默
  丢弃或越过上限；
- writer 出错时传播到完整 batch，持久化层保持 fail-stop；batch 中任何记录都
  不得在 fsync 前被确认；
- stop/drain 先拒绝新提交，再排空已接受记录、完成最终 fsync、join writer；
  shutdown 失败要可观测且不能假装成功；
- `INTERVAL` 和 checkpoint 的现有语义单独保护，不把组提交作为隐式副作用。

不得为了形成 batch 引入固定等待窗口。若等待会让单客户端延迟无界增加，方案
不成立。

## 不可妥协的不变量

1. 已确认 `SYNC` 写在恢复后不得消失；成功响应仍以覆盖该 LSN 的 fsync 为前提。
2. WAL LSN 仍严格连续，恢复仍只接受连续历史。
3. 一个 batch 内的内存 apply 和响应顺序仍按 LSN 递增，不允许跨 batch 乱序。
4. append/fsync/队列/线程失败必须 fail-stop 或明确拒绝，不能返回假成功。
5. 资源上限仍然有界；队列条目数、记录字节和未完成请求都必须有硬上限。
6. 32 个并发连接的上限不被 R4-A 改写；R4-A 不宣称超过 32 客户端的能力。
7. 进程崩溃一致性仍是验收目标；掉电、内核崩溃和设备缓存语义不在本 ADR 的
   证明范围内。

## 准入证据门槛

R4-A 只有在以下条件全部满足后才能进入实现：

### G1：可复现基线

- 使用本 ADR 对应的 v2 reliability benchmark，至少 5 轮；
- 保留原始 JSON、环境、Git HEAD、种子、轮数、客户端数和限制说明；
- `SYNC`/`INTERVAL`、1/8/32 客户端和 write/mixed/TTL 组合均有数据；
- 旧版 `reliability-20261111.json` 不被覆盖。

### G2：fsync 是主要成本

在 32 客户端 write、相同机器和构建配置下：

- 成功 `SYNC` 的 `wal_data_fsync_total_us / sync_mutation_total_us` 中位数
  至少为 **0.50**；
- `wal_append_us_mean` 和 `sync_mutation_us_mean` 足以解释 fsync 是否为
  主要剩余成本；
- 若 fsync 占比不达标，停止组提交路线，先处理其他瓶颈或重新定义 R4。

### G3：存在可用的 batch 形成机会

- 32 客户端 write 基线确认存在独立的并发连接和可排队的 SYNC 请求；
- 通过独立的、非生产路径的原型或带 instrumentation 的实验证明，在目标
  环境的一次 fsync 窗口内能稳定形成至少 **8 条**连续的待提交记录；
- 若只能形成 1 条记录，组提交没有吞吐价值，应否决；
- 该实验不得把未实现的 writer/queue 合并到主线，也不得宣称已实现组提交。

### G4：收益足够大

在相同数据集、相同机器和至少 5 轮下：

- 32 客户端 write 的 median QPS 相对当前 `SYNC` 基线提升至少 **2 倍**；
- 同时报告 P50/P99、CPU、RSS、WAL 写放大和恢复时间；
- 不能只报告客户端 QPS；不能把 QPS 提升建立在放宽 ack 持久性之上。

### G5：正确性回归

实现候选必须重新通过：

- ack set 在进程崩溃恢复后完整；
- 并发写下的 LSN 连续性、batch 边界、completion 顺序和内存 apply 顺序；
- append 失败、fsync 失败、队列满、writer 退出、owner loop 停止时的错误传播；
- 现有 C01-C14 崩溃点及新增的 batch 前后崩溃点；
- 固定 seed 随机 `SIGKILL`、尾部半写、中间损坏、manifest/segment 缺失；
- TSan、ASan、UBSan、ASan+UBSan、fuzz smoke 和格式/静态分析门禁。

### G6：有界资源与可运维性

- 队列条目数、队列字节数、batch 上限和 completion backlog 都有硬上限；
- 在 32 客户端和最大 value 下测量 RSS，不能出现无界增长；
- stop/drain 有可验证的完成条件，不依赖无限等待；
- `/stats` 能说明队列拒绝、fsync 失败和 fail-stop，而不是让客户端超时成为
  唯一观测手段。

### G7：决策记录

- 实现前更新本 ADR 状态、补充 batch/queue 的具体上限和失败矩阵；
- 若收益或正确性不达标，将本 ADR 标记为 Rejected 或 Deferred，并保留
  negative evidence；
- 不得以“已经写了线程”作为继续推进的理由。

## 明确非目标

- 本 ADR 不实现组提交、writer thread、队列、batch 或异步 completion；
- 不新增 `BATCHED` 对外模式；
- 不改变 `SYNC` 或 `INTERVAL` 的返回时机；
- 不实现后台/增量 checkpoint、事务、MVCC 或多写者；
- 不把 32 连接上限变成容量承诺；
- 不证明掉电、内核崩溃、设备缓存撒谎或介质故障；
- 不创建许可证或改变仓库许可策略。

## 被否决或暂缓的方案

- **无基线立即实现组提交**：无法区分真实瓶颈和猜测，复杂度和验证成本不可控。
- **无界队列或无限 batch**：会把 fsync 优化变成内存/延迟故障源。
- **只按固定时间等待凑 batch**：可能提高吞吐，但会让单客户端延迟不可解释。
- **只报告客户端 QPS**：不能证明 ack 后不丢或服务器端资源仍受控。
- **牺牲 ack 持久性换吞吐**：违反 `SYNC` 的核心契约。
- **把 `INTERVAL` 当成组提交**：两者都批量化 I/O，但持久化承诺不同，不能混称。

## 后果

- R4-A 当前只增加诊断能力和设计门槛，主线仍保持简单、可审计的每写一 fsync
  路径。
- 如果 G1/G2/G3 不成立，项目可以明确地停在当前架构，而不是为了“看起来更
  高级”引入不必要的并发。
- 如果进入实现，收益必须用同一环境下的可复现实验和完整正确性矩阵证明。
