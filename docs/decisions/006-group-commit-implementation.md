# ADR-006：R4-A 组提交实现

- 状态：Accepted — 实现与 G1-G7 验证完成；正式 G4 已在 clean HEAD `a06d7c2` PASS
- 日期：2026-11-11
- 关联：ADR-003、ADR-004、ADR-005、`docs/MiniDB-可靠性与持久化升级计划书.md`
- 决策范围：R4-A 的并发写路径、WAL batch 接口、checkpoint/SAVE/shutdown barrier、资源上限、失败语义和观测指标

## 背景

ADR-005 的 G1-G3 已经证明，在 32 客户端、每连接单请求在途的 closed-loop
负载下，网络请求到达率与 WAL fsync 窗口之间存在稳定的组提交机会。G3 汇总的
`pending_count >= 8` 窗口比例为 0.987605；但这只证明值得实现，不证明实现正确，
也不证明实现能达到收益门槛。

本 ADR 冻结实现选择。目标是在不放宽 `SYNC` 持久性语义、不新增公开 durability
mode、不引入固定 sleep、不绕过有界资源的前提下，把多个连续 WAL 记录合并为一次
物理 data fsync，并保持 owner EventLoop 是唯一的 admission、内存 apply、
completion 回调和协议响应点。

本 ADR 不宣称生产级，也不把基准结果外推到未测试的硬件或负载。

## 决策

### 1. 公开语义保持不变

CLI 仍只接受 `sync` 和 `interval`，不新增 `BATCHED` 枚举值、命令行选项或
隐式模式。组提交是 `SYNC` 的内部优化，不是新的持久性等级。

对于 `SYNC`：

1. 成功响应不得早于覆盖该记录 LSN 的物理 WAL batch 完成 data fsync；
2. batch 内记录按 LSN 顺序 apply 和 ack；
3. 只完成 append、未完成 fsync 的 batch 不能确认任何记录；
4. 失败 batch 不 apply、不 ack，且不得跳过其连续 LSN；
5. 一个成功响应至少对应其完整记录已经进入持久 WAL。

对于 `INTERVAL`：

1. append 成功后即可 apply 和确认 mutation；
2. data fsync 仍由 interval timer、checkpoint 或 shutdown 驱动；
3. 组提交可以合并 append，但不得把 `INTERVAL` 升级为隐式 `SYNC`；
4. interval 的完成点是 flush barrier，后续 fsync 属于 flush 阶段。

### 2. 单写者与线程边界

新增 `GroupCommitWriter`，它拥有专用 writer thread。writer 启动后：
`WalManager` 的 append、batch append、fsync、rotate、cleanup 和元数据读取由
writer thread 独占；owner 和诊断线程不得直接调用这些方法。

- owner EventLoop 是唯一 admission 点，负责参数校验、LSN 分配、提交记录、
  内存 mutation apply、调用用户 completion 和发送协议响应；
- writer thread 只消费 admission queue 从队首开始的连续 LSN 前缀，形成 batch，
  调用一次 `WalManager::append_batch`，执行必要的 data fsync，再把结果放入
  completion backlog；
- writer 不访问 `KvStore`、`KvSession`、`HttpSession` 或 socket；
- 每个完成结果只在 owner loop 应用和回调；跨线程通知必须经 owner dispatcher
  投递；
- writer 不允许按记录逐条 `queue_in_loop()`。completion backlog 只触发一个合并
  drain callback；
- writer 启动后，SAVE/checkpoint 的 rotate、flush、cleanup 也必须通过与 writer
  的 barrier 完成，不能在 owner 线程直接操作 WAL。

### 3. 状态机

正常路径：

```text
Accepted -> Queued -> Written -> FsyncSucceeded -> Applied -> Acked
```

`INTERVAL` 的 `Written` 可以直接进入 `Applied`，由后续 flush barrier 完成
`FsyncSucceeded`。失败路径为：

```text
Queued | Written -> Failed
```

持久化层故障是 fail-stop。writer 或 WAL 失败后不接受新记录，不 apply 或 ack
失败 batch，不跳过连续 LSN，不回滚已经报告成功的 LSN。崩溃恢复可以暴露一个
已经完整写入但未确认的记录；已经确认的 `SYNC` 记录不得丢失。

### 4. 有界资源

固定硬上限如下：

| 资源 | 硬上限 |
|---|---:|
| admission queue entries | 4096 |
| admission queue bytes | 64 MiB |
| 单个物理 batch records | 256 |
| 单个物理 batch bytes | 16 MiB |
| completion backlog entries | 4096 |
| completion backlog bytes | 64 MiB |

batch bytes 还受 WAL segment size 限制；不能以“batch 未满”为理由超过
`WalLimits::max_segment_bytes` 或 `WalLimits::max_total_bytes`。

队列按已接受记录的 record wire size 和固定 entry 开销计费。队列或 completion
backlog 满时，新的 admission 返回明确的 `kResourceLimit`；已接受请求不能被
静默丢弃。writer 失败时保留一个 emergency completion slot，保证所有已接受请求
的 callback 恰好调用一次。

### 5. Batch formation

- writer 从队首开始累计连续 LSN；
- 达到 256 条或编码后 16 MiB，或遇到 barrier，立即封批；
- 不等待固定 sleep 窗口，不为了凑 batch 延迟首条记录；
- 如果队首记录本身超过 16 MiB 但仍在 `WalLimits` 内，允许单记录 batch；
- 队列暂时为空时 writer 阻塞在 condition variable 上；
- batch 内任一记录无效、LSN 不连续、写入或 fsync 失败，整批进入失败路径；
- 一个物理 batch 只调用一次 `WalManager::append_batch()`；若发生 segment
  rotation，rotation 自身的必要 fsync 不受“一次 fsync”承诺约束。

### 6. 完整失败矩阵

必须覆盖并测试：

| 场景 | 行为 |
|---|---|
| admission entries 满 | 新请求返回 `kResourceLimit`，不分配 LSN，不调用 callback |
| admission bytes 满 | 同上，不截断或覆盖旧请求 |
| writer stopped/failed | 新请求返回 `kNotReady` 或 `kIoError`，不进入队列 |
| append 短写/EIO | WAL 与 writer fail-stop；整批及后续已接受请求失败，不 apply/ack |
| partial batch write | 不推进未成功记录的 LSN；恢复可能看到完整前缀，但不得看到部分记录 |
| fsync 失败 | 整批不 apply/ack；成功计数不增加；持久化层 fail-stop |
| completion entries 满 | writer fail-stop，使用 emergency slot 排空已接受 callback |
| completion bytes 满 | 同上，不允许无界增长 |
| owner stopping | 拒绝新 admission；已接受请求进入 shutdown drain |
| checkpoint barrier 失败/超时 | 不发布引用未 apply LSN 的 manifest；SAVE/shutdown 失败 |
| LSN gap/duplicate | writer 拒绝 batch 并 fail-stop，不能跳过缺口 |

callback 的恰好一次语义包括失败路径：admission 已接受后，无论成功、WAL 失败、
writer 停止还是 shutdown，都必须且只能调用一次。拒绝 admission 的请求没有
completion。

### 7. Checkpoint、SAVE 与 shutdown

SAVE、checkpoint 和 shutdown 都经过 writer barrier：

1. owner 停止或暂停新的 mutation admission；
2. writer 处理 barrier 之前所有已接受记录；
3. 对这些记录执行 append；需要持久性的 barrier 执行一次 flush；
4. completion 按 record LSN 顺序回到 owner，owner 完成 apply；
5. barrier 的 continuation 只有在目标 LSN 已 apply 后执行；
6. checkpoint 在 writer 暂停、无新记录时完成 rotate、snapshot、manifest 和
   cleanup；
7. checkpoint 成功后恢复 admission，或进入 shutdown 的最终停止阶段。

checkpoint 只能发布引用已经 apply 的 LSN。writer 已 fsync 但 owner 尚未 apply
的记录不得被 manifest 覆盖。SAVE 的网络响应不得早于 checkpoint 完成。

shutdown 状态机：

1. 拒绝新提交；
2. 排空 accepted 请求；
3. drain completion 并 apply；
4. 最终 flush/fsync；
5. checkpoint 并发布 manifest；
6. cleanup；
7. join writer thread。

shutdown 失败必须以非零进程退出码反映。不得在 owner loop 上阻塞等待一个需要
owner apply 才能完成的 completion。

### 8. 顶层 `group_commit` 指标

`GET /stats` 与 KV `STATS` 增加顶层 `group_commit` 对象。字段固定如下：

```text
queue_current_entries
queue_max_entries
queue_current_bytes
queue_max_bytes
queue_rejections
batches
batch_records
batch_bytes
max_batch_records
max_batch_bytes
batch_size_bucket_bounds
batch_size_bucket_counts
data_fsyncs
writer_loop_total_us
writer_loop_max_us
submit_to_fsync_total_us
submit_to_fsync_max_us
fsync_to_apply_total_us
fsync_to_apply_max_us
completion_backlog_current
completion_backlog_max
completion_backlog_current_bytes
completion_backlog_max_bytes
writer_failures
append_failures
fsync_failures
writer_state
```

batch-size 桶边界固定为：

```text
1, 2, 4, 8, 16, 32, 64, 128
129..256 为 overflow 桶
```

指标只记录成功操作的总量和最大值；失败由 failure counter 和 writer_state
表示。queue/backlog 的 current 与 max 必须来自同一个 writer snapshot，不能分别
读取后拼出矛盾状态。

### 9. 代码落点

- `src/storage/WalManager.{h,cpp}`：连续 LSN、batch rotation、单次尾部 fsync、
  partial write 和精确统计；
- `src/storage/GroupCommitWriter.{h,cpp}`：writer thread、有界 queue、batch
  formation、合并 completion drain、barrier、stop/join；
- `src/storage/PersistenceManager.{h,cpp}`：保留同步测试入口，增加服务器异步
  admission/completion、pending callback、checkpoint/shutdown barrier；
- `src/protocol/KvSession.{h,cpp}`：单请求在途的异步 mutation/SAVE continuation；
- `src/protocol/HttpSession.{h,cpp}`：异步 response continuation，保持
  keep-alive、pipelining 和响应顺序；
- `src/net/Acceptor.{h,cpp}`、`src/net/TcpServer.{h,cpp}`：正式停止 accepting；
- `src/net/EventLoop.{h,cpp}`：受控 signal callback；
- `src/protocol/PersistenceStatsJson.h`：稳定的 `group_commit` JSON 契约。

## 不变量

1. LSN 全局严格连续，batch 只能消费连续前缀。
2. owner 是唯一 apply 和 ack 点；completion 不得越过 LSN 顺序。
3. `SYNC` ack 不会早于覆盖该 LSN 的 fsync。
4. `INTERVAL` 不因组提交获得额外的持久性承诺。
5. 队列、batch、completion 和连接内存都有硬上限。
6. writer 或 WAL 的任何持久化失败都 fail-stop，不静默跳过。
7. checkpoint manifest 只引用已经 apply 的 LSN。
8. shutdown 失败必须反映到进程退出码。
9. 不需要也不允许修改 `LICENSE` 或 `COPYING`。

## 实现记录（2026-11-11）

已落地：

- `WalManager::append_batch()`：连续 LSN 校验、batch rotation、单次尾部 data
  fsync、partial-write 前缀和精确统计；
- `GroupCommitWriter`：专用 writer thread、有界 admission queue、严格连续
  前缀组批、合并 completion drain、pause/rotate/cleanup/resume、fail-stop；
- `PersistenceManager`：异步 mutation/checkpoint/shutdown 状态机、barrier、
  pending callback 顺序 apply、manifest LSN 边界和 writer 一致快照；
- HTTP/KV：SET、DEL、EXPIRE、POST/DELETE `/kv/...`、SAVE 均以 owner-loop
  completion 响应；断连后 completion 安全 no-op；
- EventLoop/Acceptor/TcpServer：signalfd、正式停止 accepting、graceful stop；
- `/stats` 与 `STATS`：顶层 `group_commit` 指标和 batch-size 桶。

验证已完成：实现提交 `a06d7c2` 的 GitHub Actions run `43006353520` 9/9
通过；G4 在该 clean HEAD 上采集 5 轮，median QPS **2,858.989**，相对冻结
基线 418.018 为 **6.84x**，29,100/29,100 次请求成功、0 错误。正式 artifact
为 `bench/results/r4a-group-commit-20261111.json`（28,319 bytes，SHA-256
`0ecf064ed9d7b70971188199abbc1194d974659e5ffa3ac24908b7ca9e8f8f7e`）。
G5/G6 的本地测试与资源上限验证通过；G7 由本 ADR、ADR-005 和证据文档闭环。

## 验证与准入门槛

实现必须通过现有测试、进程集成、四个 fuzz target 和格式/静态分析，并新增
batch、并发、queue full、writer stopped、partial write、fsync failure、
completion 顺序、checkpoint barrier、shutdown drain 和崩溃点测试。

实现后的本地清单为每种构建配置 **338 项 CTest**，其中
**24 个 `ProcessIntegrationTest.*`** 进程级用例。阻断式测试覆盖：

- admission queue entry/byte 上限拒绝，且被拒绝请求不调用 callback；
- completion backlog entry/byte 上限触发 fail-stop，并保证每个已接受
  callback 恰好一次；
- 一次 append 失败完成全部已接受请求，且不跳过连续 LSN；
- fsync 失败、writer stopped、cleanup 失败保持 fail-stop/paused；
- batch LSN 顺序、单批 256 records / 16 MiB 上限和 size bucket；
- HTTP/KV 异步 mutation、客户端断开 no-op、流水线响应顺序；
- SAVE/checkpoint barrier、shutdown drain 和失败非零退出码。

G4 在相同机器和数据集上执行 5 轮，32 客户端 write 的正式结果：

- median QPS **2,858.989**，高于冻结门槛 `836`，是 `SYNC` 基线 `418.018` 的
  **6.84x**；
- pooled P50/P99 为 **10,476.388/21,839.172 us**；
- client/server CPU 中位数分别为 176.739%/24.383%（单核），server RSS 峰值
  4,836 KiB；
- WAL 写放大 1.527778，恢复时间中位数 0.044825 s；
- 平均 6.696833 records/batch，queue rejection 与 writer failure 均为 0；
- 29,100/29,100 次成功、0 错误，且没有放宽 ack 持久性。

G5-G7 已按 ADR-005 完成：失败注入、崩溃恢复、网络协议、可观测性和资源上限
门槛均通过。收益必须与更高的 CPU、略高的 server RSS 和约 2 倍的恢复时间一起
引用。
