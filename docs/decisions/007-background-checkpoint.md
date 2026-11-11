# ADR-007：R4-B 后台全量 checkpoint

- 状态：Accepted，实现与本地验证进行中
- 日期：2026-11-11
- 关联：`docs/MiniDB-可靠性与持久化升级计划书.md`、`docs/crash-consistency.md`、`docs/recovery.md`、`docs/wal-format.md`
- 决策范围：R4-B 的 checkpoint 执行模型、发布边界、失败语义和观测契约

## 背景

R4-A 已把客户端写路径从 owner EventLoop 的直接 WAL fsync 改为专用 writer
thread 的组提交，但 SAVE/checkpoint 仍可能让 owner 等待完整 RDB 序列化和磁盘
写入。RDB 是最大 64 MiB、最多 100,000 records 的全量快照；即使 checkpoint
不是常规写路径，pause/drain、全量序列化和 fsync 仍会形成可见的控制面停顿。

R4-B 的目标不是增加新的持久化承诺，也不是让 checkpoint 变成自动周期任务。
它只回答一个工程问题：

> 在不改变 RDB 格式、manifest 原子发布和已确认写不丢语义的前提下，能否把
> checkpoint 中耗时最长、且不需要 owner 参与的序列化与 RDB I/O 移出 owner
> 执行路径？

可选方案有三类：

1. **后台全量 snapshot**：在短暂屏障内复制一份不可变快照，后台完成序列化、
   写入、fsync 和 rename；最终再短暂 pause 以发布 manifest 并清理 WAL。
2. **copy-on-write / 增量 checkpoint**：避免复制全量状态，但需要版本化内存
   结构、脏页跟踪、并发遍历和新的恢复边界。它会先改变存储内核，成本远高于
   当前问题。
3. **继续 stop-the-world checkpoint**：实现最简单，但无法验证控制面停顿的
   可分解性，也不能为后续 MVCC/事务 checkpoint 提供已验证的边界模型。

本 ADR 选择方案 1。

## 决策

### 1. 采用后台全量 snapshot，不做 COW 或增量 checkpoint

checkpoint 分为以下阶段：

```text
request
  -> pause writer + drain applied mutations + rotate WAL
  -> capture immutable snapshot at checkpoint_lsn
  -> resume writer and mutation admission
  -> worker: serialize -> write temp RDB -> fsync -> rename -> dir fsync
  -> final pause + drain + rotate WAL
  -> collect complete live segment list
  -> atomically publish manifest
  -> cleanup WAL prefix before retained boundary
  -> resume writer, or finish shutdown
```

`capture` 是唯一需要暂停 KV scanner 的步骤。它复制 key、value 和绝对 expiry
deadline，形成与 live `KvStore` 无关的 `RdbSnapshot`。worker 不读取 live
store，也不持有 store scanner pause。

选择全量复制的原因：

- 当前 KvStore 是单 mutex 的 `unordered_map`，没有版本链或页级脏跟踪；
- 全量复制让 checkpoint 与正常写共享现有 KvStore 语义，不需要引入新的并发
  读写协议；
- 序列化和 RDB I/O 是主要可移出路径，复制本身仍在 owner cut 内，但其成本可
  单独观测；
- COW/增量方案需要独立 ADR、独立恢复格式和新的故障矩阵，不能作为 R4-B 的
  隐式副作用。

不采用固定后台 checkpoint 周期。此阶段仍由显式 SAVE、shutdown 或既有资源
限制触发 checkpoint；自动调度留待有独立证据和准入条件时再决策。

### 2. RDB 只表示 capture LSN 的状态

设第一次 pause/drain/rotate 后的 applied LSN 为 `C`：

- snapshot 包含执行到 `C` 为止的内存状态；
- `checkpoint_lsn = C`；
- RDB 中不包含 `C` 之后的 mutation；
- worker 写 RDB 时接受的新 mutation 进入 WAL，LSN 为 `C+1...`。

RDB 的临时文件、data fsync、rename 和 parent directory fsync 继续沿用 RDB v2
的原子发布路径。manifest 未发布前，新 RDB 文件即使已经 rename 可见，也不是
权威恢复点。

### 3. manifest 记录 capture 边界和完整 live segment 集合

最终 pause 后，writer 再次 drain 并 rotate，确保 active segment 从一个明确的
LSN 开始。`checkpoint_lsn` 仍是 `C`，不是最终 `last_lsn`。manifest 的
`retained_segment_starts` 必须包含从 `C+1` 开始的全部当前 live segment，
而不是只包含 active segment。

不变量：

```text
retained_segment_starts[0] == checkpoint_lsn + 1
retained_segment_starts 严格递增
每个列出的 segment 在 manifest 发布时存在
恢复只读取这些 segment，并从 checkpoint_lsn + 1 开始重放
```

这保证后台写期间发生 WAL 轮转时，后段 WAL 不会被 manifest 或 cleanup 遗漏。

### 4. 新 RDB rename 后、manifest 发布前崩溃

恢复必须以旧 manifest 为权威。新 RDB 可能已经可见且可能包含超过旧
`checkpoint_lsn` 的状态；恢复仍会从旧 manifest 的 `checkpoint_lsn + 1`
重放旧 WAL。

该窗口是安全的，因为 RDB 写入来自捕获到 `C` 的 snapshot，而旧 WAL 包含从旧
manifest 边界到至少 `C` 的完整历史。SET、DEL 和 EXPIRE 的重放语义为：

- SET 以相同 key/value 覆盖，幂等；
- DEL 对已删除 key 仍保持删除，幂等；
- EXPIRE 持久化绝对 Unix epoch 毫秒 deadline，不保存相对 TTL，因此重放不会
  重新延长过期时间；
- 重放序列的最后一条记录仍决定最终状态。

若旧 WAL 已经清理到新 snapshot 之后，旧 manifest 不可能引用它；manifest 与
WAL 的互相校验会拒绝该状态，而不是静默接受。

### 5. manifest 发布后、cleanup 前崩溃

manifest 已 durable，因此新 checkpoint 已经生效。旧 WAL 前缀可能仍残留。恢复
只读取 manifest 列出的 segment；边界之前的 residue 被识别并忽略，不计入 live
chain。cleanup 不是 checkpoint 正确性的前提，只是空间回收。

### 6. cleanup 失败不撤销 checkpoint

cleanup 只在 manifest durable 后执行，并且只删除 start LSN 严格小于 retained
boundary 的文件。若 unlink 或目录 fsync 失败：

- manifest 已发布的 checkpoint 仍返回成功；
- writer 保持 paused，不继续在未完成清理的边界上接受写入；
- 残留 WAL 是安全的；
- 下一次 SAVE 重新 capture 并发布 manifest，然后重试 prefix cleanup；
- 下一次成功 cleanup 后才恢复 writer。

这样避免把“空间回收失败”错误升级为“已发布快照失败”，同时避免 writer 在
cleanup 未完成时继续复用旧 segment 边界。

### 7. DurabilityUnconfirmed 必须 fail closed

以下情况不能被当作普通可重试失败：

- RDB parent directory fsync 失败；
- manifest rename 后的目录 fsync 失败；
- 任何无法判断 rename 是否 durable 的错误。

此时 checkpoint 返回 `RdbSaveResult::DurabilityUnconfirmed`，manager 进入
failed 状态，writer 不恢复接受 mutation。进程只能通过重启恢复；重启时 recovery
会依据磁盘上的权威 manifest/WAL 选择状态。

### 8. 后台工作线程与 owner callback 生命周期

- 只创建一个 checkpoint worker；
- worker 只处理 serialize 和 RDB durable write；
- 完成后把结果 dispatch 回 owner；
- owner 是唯一发布 manifest、更新 checkpoint 状态和完成 SAVE callback 的线程；
- manager 析构先停止 callback state，等待已进入的 owner callback，再 join worker；
- 在析构前已经入队但尚未执行的 callback 安全 no-op，不访问半销毁对象。

## 失败矩阵

| 崩溃/失败点 | 权威状态 | 恢复结果 | 是否可继续服务 |
|---|---|---|---|
| capture 前 | 旧 RDB + 旧 manifest + 旧 WAL | 旧状态 | 可重试 |
| capture 后、worker 写入前 | 旧 RDB + 旧 manifest + 旧 WAL | 旧状态，含全部已确认 mutation | 可重试 |
| RDB temp 写入/fsync 失败 | 旧 RDB + 旧 manifest + 旧 WAL | 旧状态 | writer 恢复 |
| RDB rename 后、manifest 前崩溃 | 新 RDB + 旧 manifest + 旧 WAL | 旧 manifest 边界重放，最终状态一致 | 重启后继续 |
| manifest 部分写入 | 旧 manifest | 旧状态 | 重启后继续 |
| manifest rename 后、目录 fsync 前崩溃 | 旧或新 manifest 均必须可解释 | 任一完整 manifest 对应的 WAL 集合 | 重启后继续 |
| manifest durable、cleanup 前崩溃 | 新 manifest + WAL residue | 忽略 boundary 前 residue，重放边界后记录 | 重启后继续 |
| cleanup 失败 | 新 manifest + 部分 residue | 同上 | 本次 checkpoint 成功，writer 保持 paused，下次 SAVE 重试 |
| manifest dir fsync 返回不确定 | 可能新旧 manifest | fail closed；重启重新判定 | 否 |
| checkpoint/shutdown 并发 | 单控制操作 | 第二个请求被拒绝 | 是 |
| manager 析构时 worker 仍在运行 | 磁盘状态不变或按已发布边界 | callback no-op，worker join | 进程退出 |

## 观测契约

`GET /stats` 和 KV `STATS` 的 `persistence` 对象新增：

| 字段 | 语义 |
|---|---|
| `checkpoint_admission_blocked_total_us/max_us` | 两个 admission cut 的累计/最大阻塞时间 |
| `checkpoint_snapshot_capture_total_us/max_us` | owner 内 snapshot 复制耗时 |
| `checkpoint_snapshot_serialize_total_us/max_us` | worker 序列化耗时 |
| `checkpoint_snapshot_write_total_us/max_us` | RDB 临时文件写入与 rename 前耗时 |
| `checkpoint_snapshot_fsync_total_us/max_us` | RDB data/dir fsync 耗时 |
| `checkpoint_manifest_total_us` | durable manifest 发布累计耗时 |
| `checkpoint_wal_cleanup_total_us` | WAL prefix cleanup 累计耗时 |
| `checkpoint_total_us/max_us` | 从请求到完成的累计/最大耗时 |
| `checkpoint_failures` | 失败 checkpoint 数 |
| `background_checkpoint_state` | `idle/capture/writing/finalizing/cleanup/failed` |
| `save_client_latency_total_us/max_us` | 非 shutdown SAVE 的端到端完成耗时 |

旧字段 `checkpoint_pause_us` 保留为两个 admission cut 的兼容累计值。所有 timing
只统计成功 checkpoint；`total >= max`，且 state 必须来自同一 writer snapshot。
这些是诊断指标，不是 SLO，也不提供任意分位数。

## 已考虑的替代方案

### 继续 stop-the-world

优点是无需新增线程、无跨线程 callback 生命周期问题、无快照内存峰值。缺点是
无法回答“RDB I/O 与 owner 停顿各占多少”，也无法为后续事务 checkpoint 提供
可复用的发布边界。保留同步 `checkpoint()` 测试入口，但服务器 SAVE/shutdown
使用后台路径。

### COW / 增量 checkpoint

能降低 capture 内存和 I/O，但需要版本化数据结构、并发安全遍历、脏页集合、
增量文件格式、旧快照引用计数和新的恢复测试。它与 R4-C 的 MVCC 版本链有交叉，
应作为 MVCC 设计的一部分单独评估，而不是在 R4-B 中同时引入两个未验证变量。

### 只做后台序列化，不复制 snapshot

serialize 若直接遍历 live store，必须长时间持有 scanner pause，等价于把停顿
换到另一个线程但阻塞写者。当前选择复制后释放 pause，确保 live store 可以继续
服务。

## 后果

正面：

- owner 只承担两次短 pause/drain/rotate 和一次 snapshot 复制；
- RDB 序列化、文件写入和大部分 fsync 移出 owner；
- 后台写期间的 mutation 进入 WAL 并可恢复；
- manifest 明确保留完整 live segment 集合；
- cleanup 失败不再被误解为 checkpoint 数据失败。

代价与限制：

- capture 时存在 O(records) 内存复制和短暂 pause；
- 后台写期间 WAL 继续增长，资源上限仍可能触发拒绝或后续 checkpoint；
- 同一时间只允许一个 checkpoint；
- 不是自动 checkpoint 调度器；
- 不改变 `SYNC` / `INTERVAL` ack 语义；
- 不提供掉电、内核崩溃或设备缓存失效下的额外保证。

## 验证要求

- capture 后 writer 恢复，后台 mutation 可 apply 并在 restart 后恢复；
- 连续 SAVE 更新 manifest，第二次 capture 使用新的边界；
- 后台写期间产生多个 segment 时，manifest 保留完整集合并只清理 boundary 前文件；
- RDB rename 后、manifest 前崩溃时，旧 manifest + 旧 WAL 重放 SET/DEL/EXPIRE
  幂等；
- manifest durable 后 cleanup 前崩溃时，旧 WAL residue 被忽略；
- cleanup 失败保持 writer paused，下一次 SAVE 重试；
- `DurabilityUnconfirmed` fail closed；
- checkpoint/shutdown 竞争被拒绝；
- manager 析构后残留 callback 不访问半销毁对象；
- HTTP `/stats` 和 KV `STATS` contract 覆盖全部新字段；
- 现有 C01-C14 崩溃矩阵、四个 fuzz target、sanitizer、TSan、格式和静态分析
  全部通过。
