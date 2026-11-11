# ADR-005：R4-A 组提交候选设计与证据门槛

- 状态：Accepted — 由 ADR-006 落实实现；G1-G3 PASS，G4 clean-HEAD 证据待采集，G5/G6 本地通过
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

#### G3 冻结实验协议（正式采集前）

本协议在正式采集前固定，不能根据结果调整参数或阈值。实现位于
`bench/run_r4a_batch_formation.py`，只调用未修改的 Release
`minidb_kv_server -d sync` 和 `strace`，不进入服务器生产写路径，不增加
writer、queue、batch、completion 或 `BATCHED`。固定参数为：

| 项目 | 固定值 |
|---|---:|
| 轮数 | 5 |
| 客户端 | 32 |
| 每轮测量时长 | 2.0 s |
| 数据集 | 128 keys |
| value 大小 | 64 B |
| seed | 20261111 |
| durability mode | `SYNC` |

每轮启动全新服务器和 WAL 目录。预加载和连接预热不进入测量窗口；测量窗口内
32 个 closed-loop 客户端各自最多有一个在途 `SET`。服务器在
`strace -f -ttt -T -yy -e trace=fsync` 下运行。客户端用
`CLOCK_REALTIME` 纳秒时钟记录每个成功 `SET` 的发送开始、发送完成和收到
确认时间。只把路径匹配 `wal-[0-9]{20}\.log` 的成功 `fsync` 视为 WAL data
fsync；目录 fsync、RDB fsync、预加载内部 fsync 和 shutdown checkpoint 不进入
窗口。`strace` 的入口时间加上 `-T` duration 定义为 fsync 窗口。

主指标是保守的 `pending_count`：

~~~text
pending_count =
  count(SET where send_completed_ns <= fsync_start_ns
             and acknowledged_ns >= fsync_end_ns)
~~~

它只统计在 fsync 开始前已经完整交给内核、并且到 fsync 结束后仍未被客户端确认的
`SET`。辅助指标保留 `pending_at_start_count`、`pending_at_end_count`、
`arrivals_during_count` 和 `overlap_count`。每轮还验证：

~~~text
successes
  == measured STATS wal.appends
  == measured STATS wal.data_fsyncs
  == measured STATS persistence.sync_mutations
  == parsed WAL data fsync windows
~~~

正式 G3 PASS 必须同时满足：

| 条件 | 固定门槛 |
|---|---:|
| 轮数正好为 5 | 必须 |
| 每轮成功 `SET` | >= 400 |
| 每轮 WAL data fsync 窗口 | >= 200 |
| 总错误数 | 0 |
| 上述计数器与 trace 窗口一致 | 每轮必须 |
| 每轮 `pending_count` P50 | >= 8 |
| 每轮 `pending_count` P90 | >= 8 |
| 每轮 `pending_count >= 8` 的窗口比例 | >= 0.80 |
| 汇总所有窗口的 `pending_count >= 8` 比例 | >= 0.90 |

artifact 保留冻结 policy、Git HEAD、clean-tree 标记、环境、二进制和 harness
SHA-256、每轮客户端延迟与 fsync 延迟摘要、每个窗口的原始时间戳和五类计数，
以及每个成功请求的发送/确认时间三元组。临时 trace 文件删除，但保留其大小、
行数和 SHA-256。正式采集必须从已提交的 clean HEAD 开始；采集期间工作树变脏
即中止，已产生的部分结果不得用于 PASS。任何解析、关联或门禁代码修改后必须
重跑全部 5 轮，不能拼接旧轮次。

该实验只证明外部请求层面存在 batch 形成机会。它不证明未来服务器队列已经含有
这些记录，不证明 writer/completion 实现的开销，也不测试 G4 收益、G5 正确性、
G6 资源上限或 G7 实现记录。即使 G3 PASS，下一步也只能另写实现级 ADR，组提交
仍不得直接进入生产写路径。

#### G3 正式结果（2026-11-11）

正式采集从 clean HEAD `5d26f0be1dba0ff7e383d9660999c50204aa831f`
开始，执行 `python3 bench/run_r4a_batch_formation.py`，判定 **PASS**。
artifact 为 `bench/results/r4a-batch-formation-20261111.json`：
1,435,200 bytes，SHA-256
`f68b485204600ba83d88dd04834ee2df08595776af150cdfb164846ae6b4439d`；
harness SHA-256
`8eafe2b777de7da6413086a18e84d920375c9ea227132c9df96a98d15b77f629`；
采集所用二进制 SHA-256
`5eb7f8055489bc9bfb792a219bdc7c685760fa4020fe16ff7c15de53c2f60d38`。

5 轮共 2,985 次成功 `SET`、2,985 个 WAL data fsync 窗口、0 错误。每轮均满足
`successes == wal.appends == wal.data_fsyncs == persistence.sync_mutations
== trace windows`。汇总 `pending_count`：P50/P90/P99 均为 32，mean 31.146，
`>=8` 为 2,948/2,985 = **0.987605**。

| Round | Successful SET | Windows | P50 | P90 | `>=8` fraction |
|---:|---:|---:|---:|---:|---:|
| 1 | 597 | 597 | 32 | 32 | 0.988275 |
| 2 | 618 | 618 | 32 | 32 | 0.988673 |
| 3 | 607 | 607 | 32 | 32 | 0.988468 |
| 4 | 576 | 576 | 32 | 32 | 0.986111 |
| 5 | 587 | 587 | 32 | 32 | 0.986371 |

同次采集的 WAL data fsync 时长为 P50 1,898.000 us、P90 2,270.000 us、
P99 2,716.800 us；客户端延迟 P50 111,990.761 us、P99 133,233.279 us。
`strace` 下 median QPS 为 283.293，只用于说明采集环境，不能用于评价 G4。
实验为 WSL2 loopback、closed-loop、单客户端单在途请求的外部观测；pending
计数是批处理机会的上界，不是服务器队列已有记录的证据。实现、失败语义和
资源上限见 [ADR-006](006-group-commit-implementation.md)。

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

## 当前证据状态（2026-11-11）

- **G1：PASS。** 保留 v2 artifact
  `bench/results/reliability-20261111-r4a-baseline.json`，310,067 bytes，SHA-256
  `55bcad4ead05fcba26e44cecf17e71df6ffd7dba17e54a128398c5b65820fe84`；采集自提交 `964ba84`，工作树 clean；18 个配置各 5 轮，共
  414,688 次成功请求、0 错误。
- **G2：PASS。** 32 客户端 write 的
  `data_fsync_share_of_sync_mutation` 中位数为 **0.990576**，高于 0.50 门槛；
  `wal_data_fsync_us_mean` 中位数 1,549.798 us，
  `sync_mutation_us_mean` 中位数 1,567.930 us，
  `wal_append_us_mean` 中位数 9.238 us；每条成功 `SYNC` mutation 的 data fsync
  中位数为 1.0。
- **G3：PASS。** 独立、非生产路径的 batch formation 实验从 clean HEAD
  `5d26f0b` 采集 5 轮，共 2,985 次成功 `SET`、2,985 个 WAL data fsync 窗口、
  0 错误。每轮 `pending_count` P50/P90 均为 32，`>=8` 比例均高于 0.986；
  汇总 `>=8` 比例为 **0.987605**，高于 0.90 门槛。artifact
  `bench/results/r4a-batch-formation-20261111.json` 为 1,435,200 bytes，
  SHA-256
  `f68b485204600ba83d88dd04834ee2df08595776af150cdfb164846ae6b4439d`。
  该实验只证明外部请求层面存在 batch 形成机会。
- **G4：PENDING CLEAN HEAD。** 组提交 writer/queue/batch/completion 已由
  [ADR-006](006-group-commit-implementation.md) 实现；正式收益必须在提交并
  通过 CI 的 clean HEAD 上，用 5 轮 32 客户端 write 实验采集。
- **G5/G6：LOCAL PASS。** 338 项 CTest、24 个进程用例、故障注入、writer
  资源上限和 fail-stop 测试通过；最终证据以 clean HEAD 的完整门禁记录为准。
- **G7：IN PROGRESS。** ADR-006 已记录实现选择、失败矩阵、资源上限和代码
  落点；完成后补齐 G4 artifact 与最终状态。

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

- R4-A 的实现选择、失败矩阵和资源上限由 ADR-006 冻结；G1-G3 已 PASS，
  G5/G6 本地通过。
- 正式 G4 收益仍必须用同一环境、相同数据集和至少 5 轮的可复现实验证明；
  未达到门槛时不得把实现写成性能收益成立。
- 任何后续扩展都必须保持现有 ack 持久性语义、LSN 连续性和有界资源，不得为了
  吞吐放宽正确性。
