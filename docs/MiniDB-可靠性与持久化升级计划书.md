# MiniDB 可靠性与持久化升级计划书

> 文档状态：`R0-R3 Completed / R4-A Completed / R4-B Completed / R4-C Next`
>
> 编写日期：2026-11-10（Asia/Shanghai）
>
> 实施与本地验收：2026-11-11（Asia/Shanghai）
>
> 升级起点：`34dcc79`（`docs: translate README to Chinese`）
>
> 最近历史功能验收基线：`9da4b48`（阶段 A-H 技术验收 PASS）
>
> 计划性质：R0-R3、R4-A 和 R4-B 已按本文完成实现并通过本地验证；R4-A 的
> ADR-005、诊断指标、G3 和 clean-HEAD G4，以及 R4-B 的 ADR-007、两阶段
> checkpoint、正式 artifact 和本地六配置门禁均已记录。R4-C/R4-D 仍需遵守
> “先 ADR、后实现、每阶段独立 CI”的准入顺序。所有远程 CI 结论只引用对应
> `main` HEAD 的实际检查。
>
> 推进方式：按技术依赖和验收门槛推进，不设置固定总工期或强制周计划。阶段状态、证据链接、已知限制和下一步准入条件随实现同步更新。
>
> 验收证据入口：[可靠性证据](reliability-evidence.md)、[项目状态](PROJECT_STATE.md)、[崩溃一致性](crash-consistency.md)、[恢复模型](recovery.md)。

关联文档：

- [项目状态](PROJECT_STATE.md)
- [系统架构](architecture.md)
- [RDB v2 格式](rdb-format.md)
- [阶段 A-H 历史验收报告](A_H_ACCEPTANCE_REPORT.md)
- [原实施计划书（历史）](MiniDB-实施计划书.md)
- [技术评审（历史）](MiniDB-技术评审.md)
- [WAL 与 manifest 格式](wal-format.md)
- [恢复模型](recovery.md)
- [崩溃一致性](crash-consistency.md)
- [可靠性证据](reliability-evidence.md)
- [ADR-005：R4-A 组提交候选设计与证据门槛](decisions/005-group-commit.md)
- [ADR-006：R4-A 组提交实现](decisions/006-group-commit-implementation.md)
- [ADR-007：R4-B 后台全量 checkpoint](decisions/007-background-checkpoint.md)

## 目录

- 0. 执行摘要
- 1. 背景与项目判断
- 2. 新定位与范围
- 3. 目标架构
- 4. 持久化语义
- 5. WAL 设计
- 6. 崩溃一致性与故障注入
- 7. 性能、恢复与证据
- 8. 分阶段实施计划
- 9. 风险与应对
- 10. 对外表达、简历与面试
- 11. 阶段完成定义
- 12. 下一步决策
- 13. 变更记录

---

## 0. 执行摘要

MiniDB 已经完成了阶段 A-H 的技术验收，具备较完整的网络层、KV/TTL 存储和 RDB 快照能力。当前问题不是“代码量不够”或“测试不够多”，而是：

> **项目的核心机制仍然是一个常见的 mini KV + RDB 快照；真正较强的部分集中在验证和故障测试，而不是一个足够硬、可持续追问的存储问题。**

因此，本计划建议：

1. 取消“教学型”定位；
2. 不宣称“生产级数据库”；
3. 将项目重新定位为：

> **面向有界资源的单机 crash-consistent KV 存储服务。**

4. 以 WAL、RDB checkpoint、明确持久化语义和崩溃注入矩阵为主线；
5. 用可复现实验证明：哪些写成功后一定可恢复，哪些可能丢失，不同持久化级别的性能与恢复代价是什么。

推荐路线：

```text
R0 重新定位与证据规范
  -> R1 WAL + RDB checkpoint 正确性基线
  -> R2 崩溃一致性与恢复模型闭环
  -> R3 持久化性能与公开证据
  -> R4 组提交、后台 checkpoint 或事务能力（可选，证据驱动）
```

本计划明确不包含：

- SQL 解析器/执行器；
- 完整 Redis 兼容；
- Raft、复制和集群；
- 事务/MVCC 的立即实现；
- TLS、认证授权和多进程共享存储；
- “生产级”“高并发”“高吞吐”等未经证据支持的宣传。

---

## 1. 背景与项目判断

### 1.1 历史目标

历史对话中，MiniDB 的原始定位是：

> “基于 C++ 的网络通信与单机 KV 存储服务。”

原始目标同时明确要求：

- 不是快速堆出一个 Demo；
- 要形成可以公开展示、经得起技术面试追问的项目；
- 重点是 C++ 工程化、Linux Socket、epoll、Reactor、协议、存储、持久化、测试和性能证据；
- 不要求 Raft、多节点复制、复杂事务或生产级 Redis 兼容。

“教学型数据库”主要是后续文档中的自我描述，不是架构本身的限制。取消这个标签是合理的，但不能只做文字替换。

### 1.2 当前项目画像

| 维度 | 当前情况 | 评价 |
|---|---|---|
| 网络 | 单 EventLoop、水平触发 epoll、非阻塞 TCP、32 连接上限、每连接有界缓冲 | 完成度较高，但属于常见系统项目能力 |
| 协议 | 严格 HTTP/1.1 子集和二进制 KV 协议 | 测试充分，但不是主要差异化来源 |
| 存储 | `unordered_map` + 单 `mutex` + TTL | 正确性可解释，核心算法较常见 |
| 持久化 | RDB v2 全量快照、CRC32C、临时文件、fsync、原子 rename、目录 fsync | 工程质量强，但只有全量快照 |
| 恢复 | 启动时加载最近一次 RDB；损坏时拒绝发布 | 没有基于 WAL 的崩溃恢复 |
| 资源边界 | 32 连接、每连接 1.25 MiB 输入/输出预算、64 MiB RDB 上限 | 已有“有界资源”的雏形，应上升为项目命题 |
| 测试 | 计划立项时历史基线为六配置各 214/214 CTest；R0-R3 为六配置各 305/305；R4-A 为六配置各 338/338；R4-B 为六配置各 348/348，含 25 个进程用例和四个 20,000-run fuzz target | 已形成可复现的工程证据 |
| 故障注入 | RDB EINTR、短写、ENOSPC、open/fsync/close/rename/dir-fsync 等 | 可复用为 WAL 崩溃矩阵的基础 |
| 性能 | HTTP 约 1,782 req/s、KV 约 1,968 req/s，单连接、单 in-flight、closed-loop | 只能作为基线，不能作为容量或生产 SLA 结论 |
| 主要缺口 | 无 WAL/AOF；无 ack 后崩溃一致性；SAVE stop-the-world；无事务、复制、多进程所有权 | 缺少一个真正硬的存储可靠性闭环 |

一个关键事实是：64 MiB RDB 快照约耗时 1.265 秒，峰值 RSS 约 200.8 MiB，并且 SAVE 会阻塞 owner loop。没有 WAL 时，全量快照既是持久化机制，也是唯一恢复来源；有 WAL 后，RDB 应转为 checkpoint，用来限制恢复时间，而不是承担每一次写入的可靠性承诺。

### 1.3 真正的问题

从外部 reviewer 的视角，当前项目容易被概括为：

> “一个 C++ 写的 mini Redis，加了一个 mini HTTP server，再配了一套不错的测试。”

这不是因为实现质量差，而是因为：

- 核心数据结构常见；
- 持久化是标准全量快照；
- 项目的主要优势在“验证”，但验证围绕的是常见功能；
- README 首先声明“教学用途”，读者会在看到测试证据之前先完成项目归类。

可以用一句话概括：

> **当前项目“验证强、机制轻”。**

因此，升级方向不是增加更多命令，而是增加一个可以被验证的硬属性：

> **已确认写在崩溃后是否一定可恢复，以及不同持久化级别的代价边界。**

### 1.4 为什么不能直接改称“生产级”

如果只是把“教学型”换成“生产级数据库”，会立刻面临以下追问：

- 有没有 WAL 和崩溃恢复？
- 有没有 TLS、认证、授权？
- 有没有复制、一致性或故障切换？
- 多个进程能否共享数据目录？
- 长期运行、磁盘满、日志损坏如何处理？
- 性能数据是否代表饱和容量？
- 有没有恢复时间、写放大和尾延迟数据？

当前项目对这些问题还没有完整答案。直接宣称生产级，会把项目从“诚实的工程原型”变成“无法自圆其说的包装”。

正确做法是：

> **不宣称生产级，但把可靠性工程做到足以被严肃讨论。**

---

## 2. 新定位与范围

### 2.1 目标定位

建议采用以下一句话定位：

> **MiniDB 是一个面向有界资源的单机 KV 存储服务，核心验证从网络请求到崩溃恢复的持久化闭环。**

英文可用：

> **A crash-consistent single-node KV store for bounded-resource environments.**

这里的“有界资源”指：

- 单节点、单进程；
- 有限内存；
- 有限连接数；
- 有界的每连接缓冲；
- 有界的快照和 WAL 大小；
- 明确的恢复时间和恢复行为；
- 不把“横向扩展”作为当前目标。

### 2.2 核心问题

项目应围绕三个问题组织：

1. **客户端收到写成功响应后，进程在任意点崩溃，数据是否一定还在？**
2. **不同持久化级别下，允许丢失多少最近写，恢复时间是多少？**
3. **恢复过程如何区分尾部半写、日志损坏、快照缺失和正常日志，并拒绝静默数据损坏？**

面试时可以把项目描述为：

> “我不是又写了一个 Redis，而是围绕‘已确认写不丢’这个不变量，做了 WAL、checkpoint 和崩溃注入。”

### 2.3 设计原则

1. **正确性先于性能。** 先证明最严格的持久化语义，再讨论组提交和吞吐。
2. **不变量先于功能。** 每个新增机制都必须对应一个可测试的系统性质。
3. **证据先于宣传。** README 中的每个能力声明都应有测试、报告或原始数据支撑。
4. **复用已有资产。** RDB v2、故障注入、CTest、fuzz 和 CI 是资产，不重写。
5. **保持有界资源。** WAL、segment、恢复内存和未完成请求都要有上限。
6. **明确不保证什么。** 未确认写、损坏日志、磁盘错误和进程并发访问都必须有清晰语义。

### 2.4 明确的非目标

本阶段不做：

- SQL 解析、执行计划、优化器；
- Redis 完整命令集和协议兼容；
- Raft、复制、集群、分片；
- 事务、MVCC、隔离级别；
- TLS、认证和授权；
- 多进程共享同一数据目录；
- 生产 SLA、跨平台生产验证；
- 以 io_uring、多 Reactor 或零拷贝作为主卖点；
- 在没有实测数据的情况下宣传“高吞吐”或“高并发”。

这些不是永远不能做，而是不能抢占当前可靠性主线。

---

## 3. 目标架构

### 3.1 当前架构与目标架构

| 维度 | 当前 | 目标 |
|---|---|---|
| 写入 | 修改内存，SAVE 时全量快照 | WAL 先记录，按持久化级别落盘，再应用内存 |
| 成功响应 | 请求完成即返回；持久化由 SAVE 负责 | 由持久化级别决定，`SYNC` 在 fsync 后返回 |
| 持久化 | RDB 是唯一持久化机制 | WAL 负责近期写，RDB 作为 checkpoint |
| 恢复 | 只加载最近快照 | 快照 + WAL 重放，恢复到最后合法前缀 |
| 故障边界 | 主要覆盖 RDB 写盘失败 | 覆盖 WAL、快照、manifest、目录 fsync、随机 kill |
| 证据 | RDB 故障注入和功能测试 | WAL/checkpoint 崩溃矩阵和恢复模型测试 |
| 性能叙事 | 单连接 closed-loop 基线 | 持久化级别、恢复时间和 checkpoint 停顿的权衡曲线 |

### 3.2 目标写路径

```text
Client
  -> KV/HTTP Session
  -> 命令校验
  -> WAL 追加
  -> 按 durability policy fsync
  -> 应用到 KvStore
  -> 返回成功响应

RDB checkpoint：
KvStore snapshot + checkpoint_lsn
  -> 临时快照写入/fsync/rename/目录 fsync
  -> manifest 原子发布
  -> 删除不再需要的 WAL segment
```

### 3.3 目标恢复路径

```text
启动
  -> 获取数据目录所有权锁
  -> 读取 manifest
  -> 加载 RDB 快照到临时 KvStore
  -> 按 LSN 顺序扫描 WAL segments
  -> 校验记录、拒绝中间损坏
  -> 截断最后一条不完整尾部记录（若允许）
  -> 重放 checkpoint_lsn 之后的记录
  -> 验证通过后发布 Store
  -> 开始监听
```

只有完整恢复成功后，服务器才允许开始接受请求。不能一边服务一边重放，也不能把部分恢复结果暴露给客户端。

### 3.4 建议模块边界

具体类名可在 ADR 中调整，但建议保持以下职责分离：

| 模块 | 职责 |
|---|---|
| `WalFormat` | WAL 记录编码/解码、长度和 CRC 校验 |
| `WalFileOperations` | 可注入的 open/write/fsync/rename/unlink 抽象，复用 RDB 故障注入模式 |
| `WalManager` | 追加、segment 轮转、fsync 级别、LSN 分配 |
| `WalReplay` | 扫描、校验、尾部截断、按 LSN 重放 |
| `CheckpointManifest` | manifest 编码/解码、checkpoint_lsn、segment 列表 |
| `RecoveryManager` | 组合 RDB、manifest 和 WAL，完成启动恢复 |
| `DurabilityPolicy` | 公开 `SYNC`、`INTERVAL` 语义和返回时机，以及 `SYNC` 内部组提交策略 |

网络层不应直接依赖 WAL 细节；存储模块也不应依赖网络模块。HTTP 和 KV 两个服务应复用同一套存储恢复逻辑。

### 3.5 线程模型

**R1 正确性基线：**

- 保持当前单 owner EventLoop；
- 在 owner loop 内同步完成 WAL 追加和 fsync；
- 这会阻塞事件循环，但实现简单、顺序清晰，适合先建立正确性基线；
- 该阶段的吞吐不是最终目标，必须如实记录 fsync 带来的停顿。

**R4-A 已实现优化：**

- R1 benchmark 与 G1-G3 证明 fsync 是 `SYNC` 写路径的主要瓶颈后，才引入专用 WAL writer thread 和组提交；
- 写请求按顺序进入有界队列，WAL writer 批量 append + 一次 data fsync；
- fsync 完成后通过有界 completion backlog 回到 owner loop，再按原顺序应用内存并响应；
- 组提交只优化“多个写共享一次 fsync”，没有改变全局应用顺序和 ack 语义；
- eventfd、有界队列/backlog、stop/drain、失败 fail-stop 和 TSan 测试均已落地。

**checkpoint 策略：**

- R1 继续使用 stop-the-world checkpoint，保持当前可证明的线性化点；
- 只有测出 checkpoint 停顿不可接受后，才考虑后台快照、增量 checkpoint 或 copy-on-write；
- 不提前引入复杂并发快照。

### 3.6 建议的文件布局

为了兼容当前 `-r <rdb_path>` 命令，P0 建议保留 RDB 路径，并在其旁边增加 sidecar：

```text
<rdb_path>                 # 当前 RDB v2 快照
<rdb_path>.manifest        # checkpoint 元数据，原子替换
<rdb_path>.wal/            # WAL segment 目录
<rdb_path>.lock            # 单写者所有权锁（建议）
```

WAL segment 示例：

```text
wal-000001-0000000000000001.log
wal-000001-0000000000100001.log
```

manifest 至少需要包含：

- magic/version；
- `checkpoint_lsn`；
- snapshot generation 或 snapshot 文件名；
- WAL segment 列表及每个 segment 的起始 LSN；
- CRC32C；
- 未来扩展字段的保留位。

具体二进制布局由 `ADR-002` 定稿。核心要求是：

> **快照与 WAL 的关联必须通过原子发布的 manifest 完成，不能靠文件名猜测，也不能在启动时静默跳过无法解释的日志。**

RDB v2 文件格式可以保持不变，`last_lsn` 放在 manifest 中，避免立即升级 RDB 版本。若后续需要 generation snapshot 或独立 data directory，再单独做 ADR。

### 3.7 兼容与迁移

- 只有旧 RDB、没有 manifest 和 WAL：允许一次性 legacy import，加载 RDB 后创建空 WAL/manifest，`checkpoint_lsn = 0`。
- 只有旧 RDB、但存在 WAL：拒绝自动迁移，避免跳过未知日志。
- manifest 存在但快照缺失：拒绝启动。
- manifest 损坏或 WAL 中间损坏：拒绝启动，不能部分发布。
- 旧版 RDB 的 64 MiB、100,000 records、key/value 上限继续作为兼容边界。

引入单写者锁（例如对 `<rdb_path>.lock` 使用 `flock(LOCK_EX | LOCK_NB)`）是建议的 P0 安全项。当前文档明确没有多进程文件锁；WAL 会让两个写者共享目录变得非常危险，不能只靠“请勿同时运行”的口头约定。

---

## 4. 持久化语义

### 4.1 术语

| 术语 | 含义 |
|---|---|
| acknowledged write | 服务已经向客户端返回成功响应的写 |
| durable write | 在给定存储假设下，崩溃和重启后仍能恢复的写 |
| valid prefix | WAL 从起点开始、连续且校验通过的一段记录 |
| torn tail | 文件末尾写到一半或未完成 fsync 的最后一个记录 |
| checkpoint | 将某一 LSN 之前的完整状态写成 RDB 快照 |
| manifest | 原子发布快照和 WAL 关系的元数据 |
| LSN | 单调递增的日志序列号，用于排序和恢复截断 |

### 4.2 持久化级别

| 模式 | 成功返回条件 | 已确认写保证 | 可能丢失 | 用途 |
|---|---|---|---|---|
| `SYNC` | WAL append + fsync 成功，内存应用后 | 在 fsync 假设下，已确认写不丢 | 无（对已确认写） | R1 正确性基线 |
| `BATCHED`（未公开） | 同一批次 WAL fsync 成功，按顺序应用后 | 同 `SYNC` | 无（对已确认写） | 仅作为 R4-A 内部组提交；不提供 CLI 模式 |
| `INTERVAL` | 内存应用后，周期 fsync | 不保证崩溃后不丢 | 最近一个 fsync 窗口 | 明确的弱持久化对照 |
| `NONE` | 内存应用后 | 无 | 进程崩溃也可能丢 | 仅基准对照，不作为对外能力 |

默认建议：

- R1 默认 `SYNC`；
- `INTERVAL` 必须显式配置，不能默认开启；
- 不新增公开 `BATCHED` 模式；R4-A 组提交只作为 `SYNC` 的内部优化存在；
- README 必须明确每个模式的保证和代价，不能只列模式名。

### 4.3 核心不变量

1. **I1：ack 后持久性。** `SYNC`（含内部组提交）下，客户端收到成功响应后，已确认写必须在恢复后可读；前提是存储设备遵守 fsync 语义。
2. **I2：合法前缀。** 恢复只接受连续、校验通过的 WAL 前缀；尾部半写可截断，中间损坏必须拒绝启动。
3. **I3：重放幂等。** 快照与 WAL 的重放不能重复应用或漏应用；同一 LSN 不能出现两次不同语义。
4. **I4：TTL 语义稳定。** WAL 保存绝对 deadline，重放不受重启时间和相对时钟影响。
5. **I5：不安全状态 fail-stop。** WAL 追加/fsync 失败、manifest 损坏或中间日志损坏时，不能继续返回写入成功。
6. **I6：有界恢复。** 恢复内存、WAL 总大小、segment 数量和单条记录长度必须有上限。
7. **I7：未确认写允许出现。** 崩溃前未返回成功的写，恢复后可能出现；这不违反“已确认写不丢”，但必须有测试和文档说明。

### 4.4 故障处理策略

| 故障 | 内存/状态处理 | 客户端结果 | 重启行为 |
|---|---|---|---|
| WAL append 失败 | 不应用新写；停止接受 mutation 或进入 degraded 状态 | 不返回成功 | 截断尾部或拒绝启动 |
| WAL fsync 失败 | 不返回成功；标记存储不可信 | 错误或超时 | 依据尾部校验决定 |
| 内存应用失败（fsync 后） | 记录可能已经持久化 | 不返回成功 | 允许重放出现该写 |
| checkpoint 失败 | 保留 WAL；旧 manifest 继续有效 | SAVE 返回错误 | 从旧快照 + WAL 恢复 |
| WAL 中间记录损坏 | 不发布部分恢复结果 | 启动失败 | 保留现场，人工诊断 |
| WAL 尾部半写 | 截断到最后合法记录 | 启动继续 | 记录 warning |
| manifest 缺失但有 WAL | 不自动猜测 | 启动失败 | 人工恢复或明确迁移 |
| 磁盘满/EIO | 停止写入并报告错误 | 不返回成功 | 依故障阶段恢复 |

核心规则：

> **没有成功 fsync，就不能向客户端承诺持久化成功。**

### 4.5 未确认写

测试模型必须区分：

- **ack set**：客户端已经收到成功响应的操作集合；
- **possible set**：崩溃前后可能已经写入磁盘的操作集合。

允许：

```text
possible set 中出现未确认写
```

不允许：

```text
ack set 中任一操作在恢复后缺失
```

这比简单地比较“崩溃前后全部数据完全一致”更准确，也更适合真实崩溃语义。

---

## 5. WAL 设计

### 5.1 逻辑日志

R1 建议使用逻辑日志，而不是页级物理日志：

- `SET`：记录 key、value 和绝对 expiry；
- `DEL`：记录显式删除；
- `EXPIRE`：记录新的绝对 deadline；
- `GET`、`TTL`、`PING`、`SAVE` 不写 WAL；
- 对不存在 key 的 `DEL`/`EXPIRE` 不产生状态变化，可不记录；
- TTL scanner 的惰性删除不作为独立日志记录，依靠绝对 deadline 在重放时收敛。

逻辑日志适合当前单写者、顺序应用和简单状态机。未来引入并发事务后，必须重新评估日志顺序和隔离语义。

### 5.2 建议记录格式

以下是 R1 的建议格式，具体字节布局由 `ADR-002` 固化：

| 字段 | 大小 | 说明 |
|---|---:|---|
| magic | 4 | `MWAL` |
| version | 2 | v1 |
| flags | 2 | v1 必须为 0，未知 flag 拒绝 |
| lsn | 8 | u64 BE，单调递增 |
| op | 1 | SET=1，DEL=2，EXPIRE=3 |
| reserved | 1 | 0 |
| key_len | 4 | u32 BE |
| value_len | 4 | u32 BE |
| expiry_ms | 8 | i64 BE，绝对 Unix ms；0 表示无过期 |
| key | variable | 1..1024 bytes |
| value | variable | 0..1 MiB bytes |
| crc32c | 4 | 覆盖本记录除 CRC 外的所有字节 |

约束：

- `SET` 的 value 可以为空；
- `DEL` 的 `value_len` 必须为 0；
- `EXPIRE` 的 `value_len` 必须为 0；
- `expiry_ms` 必须非负；
- 单条记录最大长度必须可计算且有硬上限；
- LSN 必须严格递增；
- 未知 op、未知 version、未知 flag、长度越界、CRC 错误均拒绝；
- 不允许把损坏记录当作“空白”跳过。

### 5.3 LSN 与顺序

- 每个 mutation 分配一个单调递增 LSN；
- LSN 在内存中全局有序，单 owner loop 下与命令执行顺序一致；
- WAL segment 文件名包含起始 LSN；
- manifest 记录 `checkpoint_lsn`；
- 恢复时只重放 `LSN > checkpoint_lsn` 的记录；
- 重启后 `next_lsn = max(最后合法 LSN, checkpoint_lsn) + 1`；
- LSN 溢出必须 fail-stop，不能回绕。

### 5.4 写路径

R1 的 `SYNC` 写路径：

```text
1. 校验命令和资源上限
2. 构造 WAL record
3. 追加到 active segment
4. fsync WAL 文件
5. 应用到 KvStore
6. 返回成功
```

如果第 4 步失败，不能继续返回成功。
如果第 5 步失败，WAL 可能已经有记录，客户端会收到错误，但重启后该写可能出现；这是允许的未确认写。

### 5.5 恢复算法

1. 获取单写者锁。
2. 读取 manifest；缺失时走 legacy import 或拒绝启动。
3. 加载 RDB 快照到临时 KvStore。
4. 按 segment 起始 LSN 排序。
5. 逐条读取和校验 WAL record。
6. 只重放 `LSN > checkpoint_lsn` 的记录。
7. 最后一条记录不完整时，截断到最后一个合法记录，并 fsync 文件和目录。
8. 中间记录损坏、LSN 断层、重复 LSN 或段范围不一致时拒绝启动。
9. TTL 依据绝对 deadline 处理，过期项不得复活。
10. 全部成功后，才原子发布 Store 并开始监听。

恢复过程必须记录：

- 加载的快照；
- 重放 segment 数量和记录数；
- 截断的尾部位移；
- 恢复耗时；
- 最终 `last_lsn`。

### 5.6 Checkpoint 与 manifest

建议的 checkpoint 顺序：

1. 停止或冻结 mutation admission；
2. 确定 `checkpoint_lsn`；
3. 将 active WAL segment 轮转为不可变 segment，并 fsync；
4. 锁定 Store，导出快照；
5. 写快照临时文件，fsync，rename，fsync 父目录；
6. 写 manifest 临时文件，fsync，rename，fsync 父目录；
7. 删除 manifest 不再引用的旧 segment，并 fsync 目录；
8. 恢复 mutation。

关键规则：

- snapshot 必须在 manifest 之前发布；
- manifest 只能引用已经持久化的 snapshot 和 segment；
- WAL 清理只能发生在 manifest 成功发布之后；
- checkpoint 失败不能删除 WAL；
- 崩溃后允许留下“新快照 + 旧 manifest”，但恢复必须通过 LSN 重放和幂等性测试证明不会重复应用或丢数据；
- 如果后续认为该依赖难以解释，再引入 generation snapshot，以 manifest 指针切换完整快照。

R1 的 checkpoint 仍可以是 stop-the-world；先保证正确性，再根据测量结果决定是否做后台 checkpoint。

### 5.7 TTL 语义

- WAL 保存绝对 `expiry_ms`，不保存“剩余 TTL”；
- `EXPIRE` 在写入时计算并固化 deadline；
- 重放时按照绝对时间判断是否过期；
- 快照恢复和 WAL 重放必须使用同一套 TTL 判定逻辑；
- 墙钟跳变的影响必须记录在文档和测试中；
- 测试覆盖：重启、过期、过期后重建、scanner 与命令竞争、墙钟回拨/跳跃。

### 5.8 资源上限

必须配置并测试：

- 单个 WAL segment 最大字节数；
- 所有 WAL segment 总字节数软/硬上限；
- 单条记录最大字节数；
- 恢复时可分配的最大内存；
- group-commit admission queue 与 completion backlog 的条目/字节上限（已实现）；
- checkpoint 临时文件大小；
- 启动恢复允许的最大 segment 数量。

达到上限时的策略必须明确：拒绝写、触发同步 checkpoint、返回错误或 fail-stop。不能因为 WAL 无限增长而把磁盘写满。

### 5.9 需要在 ADR 中定稿的问题

- manifest 的精确二进制布局；
- `-r <rdb_path>` sidecar 方案是否保留，或新增 `--data-dir`；
- WAL segment 大小和轮转策略；
- `SYNC` 默认值与 CLI 配置形式；
- WAL 记录是否包含 header CRC 和 payload CRC 分离；
- WAL segment 清理的精确顺序；
- 是否在 R1 就加入 `<rdb_path>.lock`；
- `SAVE` 是否同时触发 checkpoint 和 WAL 清理；
- 是否增加 `INFO`/`STATS` 命令暴露 LSN、WAL 大小和 durability mode；
- WAL 损坏时的诊断命令和恢复工具边界。

---

## 6. 崩溃一致性与故障注入

### 6.1 崩溃点矩阵

至少覆盖以下场景：

| 编号 | 崩溃/故障点 | 验证目标 |
|---|---|---|
| C01 | WAL 追加前 | 原状态不变，无成功响应 |
| C02 | WAL header 半写 | 尾部截断，启动成功 |
| C03 | WAL payload 半写 | 尾部截断，启动成功 |
| C04 | 完整记录写入但未 fsync | 未确认写可缺失，已确认写不丢 |
| C05 | fsync 后、内存应用前 | 记录可被重放，允许未确认写出现 |
| C06 | 内存应用后、响应前 | 已收到 ack 的写必须恢复 |
| C07 | 快照临时文件写入中 | 旧快照/manifest/WAL 仍可恢复 |
| C08 | 快照 fsync 后、rename 前 | 旧快照继续有效 |
| C09 | 快照 rename 后、manifest 更新前 | 通过 LSN 重放保持正确 |
| C10 | manifest temp 写入中 | 旧 manifest 继续有效 |
| C11 | manifest rename 后、目录 fsync 前 | 旧/新 manifest 均可解释，不丢已确认写 |
| C12 | WAL segment 删除过程中 | manifest 引用完整，恢复不依赖已删除数据 |
| C13 | 随机时刻 `kill -9` | 已确认写不丢，未确认写可缺失/出现 |
| C14 | ENOSPC、EIO、短写、EINTR | 错误不静默，状态可诊断 |

每个崩溃点必须记录：

- 客户端是否收到成功响应；
- 内存是否已应用；
- WAL 是否完整；
- 快照/manifest 是否可见；
- 重启后状态；
- 允许出现的差异集合。

### 6.2 恢复模型测试

建议实现一个模型测试：

1. 生成随机操作序列（SET/DEL/EXPIRE）和随机 key/value；
2. 记录每个操作是否收到成功响应；
3. 在指定崩溃点或随机时刻终止进程；
4. 重启并读取恢复后的状态；
5. 验证：
   - 所有 ack 操作对应的最终状态存在；
   - 未 ack 操作可以选择存在或不存在；
   - 不存在部分 key/value；
   - TTL 语义与绝对 deadline 一致；
   - 不出现已删除 key 被旧日志复活；
6. 使用固定 seed 复现失败，保存操作日志和现场文件。

### 6.3 Fuzz 与静态检查

- WAL record parser fuzz：随机长度、随机 CRC、截断、非法 version/flags/op、超大长度；
- manifest parser fuzz：随机字节、非法 segment 引用、CRC 错误、重复 LSN；
- 继续运行现有 HTTP/KV fuzz；
- ASan/UBSan 覆盖 WAL 编解码和恢复路径；
- TSan 覆盖已实现的组提交；后台 checkpoint 若后续实现也必须纳入；
- clang-format、clang-tidy 和现有 CI 矩阵不能回退。

### 6.4 CI 分层

- 快速单元测试：每次提交运行；
- WAL 恢复集成测试：每次提交运行；
- 进程级崩溃矩阵：PR/主分支运行；
- 随机 kill -9 模型测试：定时或手动触发，保存 seed 和 artifact；
- 性能测试：独立任务，不与功能测试混在同一时间预算中。

---

## 7. 性能、恢复与证据

### 7.1 需要回答的问题

性能工作不是追求一个孤立 QPS 数字，而是回答：

1. `SYNC`（含内部组提交）与 `INTERVAL` 的吞吐和尾延迟差异是多少？
2. 达到同样 ack 语义时，组提交能提升多少？
3. WAL 大小增长如何影响恢复时间？
4. checkpoint 的停顿、RSS 和写放大是多少？
5. 1、8、32 个客户端下的变化是什么？
6. 这些结果在 WSL2 同机 loopback 环境下有哪些限制？

### 7.2 基准维度

| 维度 | 取值 |
|---|---|
| 持久化级别 | `SYNC`（含内部组提交）、`INTERVAL`，以及未实现组提交的 v2 baseline |
| 客户端数 | 1、8、32；不夸大超过连接上限的扩展性 |
| 工作负载 | 小 key/value 写、混合读写、TTL 负载、重启恢复 |
| 指标 | throughput、P50/P95/P99/P999、错误率、CPU、RSS、WAL bytes、fsync 次数 |
| 恢复 | 记录数、WAL 字节数、恢复耗时、截断次数 |
| checkpoint | 停顿时间、RSS、快照大小、WAL 清理量 |
| 可复现性 | 固定 seed、至少 5 轮、记录中位数和波动 |

### 7.3 报告要求

- 保存原始脚本和原始输出；
- 记录系统、编译器、CMake、CPU、文件系统和 WSL 版本；
- 明确说明 closed-loop、单机、loopback、连接上限等限制；
- 不把单连接结果写成“容量”；
- 不虚构 QPS、P99 或恢复时间；
- 失败结果和异常样本也要保留；
- README 只引用报告中真实存在的数字。

### 7.4 建议的验收证据

- WAL/恢复专项测试清单；
- 崩溃矩阵结果表；
- 随机 kill 的 seed 和 artifact；
- 恢复时间与 WAL 大小曲线；
- checkpoint 停顿数据；
- `SYNC`/`INTERVAL` 两种公开模式与 `SYNC` 内部组提交优化的对比；
- R1 验收时为 305 项，R4-A 后增至 338 项，并在六种配置下无回退；
- CI、ASan/UBSan/TSan 和四个 fuzz target 的结果。

---

## 8. 分阶段实施计划

阶段编号使用 `R`（Reliability），与已完成的 A-H 功能验收区分。

### R0：重新定位与文档基线

**目标：** 去掉“教学型”标签，建立新的技术命题和证据规范，不修改运行时代码。

**任务：**

- [x] 修改 `README.md` 顶部的项目定位；
- [x] 删除或替换 README 中的“教学用途”表述；
- [x] 更新 `docs/architecture.md` 的 Scope；
- [x] 更新 `docs/deployment.md` 的开头定位；
- [x] 更新 `docs/PROJECT_STATE.md`，加入新阶段和当前状态；
- [x] 在 README 文档列表中加入本计划书和后续 WAL 文档；
- [x] 新增 `docs/decisions/002-wal-and-crash-consistency.md`；
- [x] 新增 WAL 格式与恢复语义草案；
- [x] 新增“声明-证据-限制”表；
- [x] 保留“不是生产数据库”的诚实声明；
- [x] 历史计划书标记为历史/归档，不强行改写全部内容。

**验收门槛：**

- README 前 30 行能说清：项目是什么、核心问题是什么、证明了什么、不保证什么；
- 公开入口文档不再把“教学用途”作为项目身份；
- 没有把项目描述成生产级、Redis 兼容或高并发系统；
- 所有能力声明都能指向测试、报告或明确的待验证状态。

### R1：WAL + RDB checkpoint 正确性基线

**目标：** 在单写者、单 owner loop 的前提下，实现最小但语义完整的 WAL 和恢复路径。

**任务：**

- [x] 固化 WAL 记录格式和 manifest 格式；
- [x] 实现 WAL 编解码、CRC、LSN 和边界校验；
- [x] 实现可注入 WAL 文件操作；
- [x] 实现 WAL 追加、segment 轮转和 fsync；
- [x] 实现启动恢复和尾部半写截断；
- [x] 实现 RDB checkpoint 与 manifest 原子发布；
- [x] 将 `SAVE`、优雅关闭接入 checkpoint；
- [x] 增加 `SYNC` durability 配置；
- [x] 增加旧 RDB 的 legacy import 路径；
- [x] 增加单写者锁或明确启动所有权检查；
- [x] 增加 WAL/恢复专项测试；
- [x] 保持现有 HTTP/KV 行为和六配置测试不回退。

**验收门槛：**

- `SYNC` 模式下，已确认写在崩溃后不丢；
- 尾部半写可恢复，中间损坏拒绝启动；
- 快照和 WAL 的重放不重复应用、不漏应用；
- `SAVE` 失败不会导致 WAL 被提前删除；
- R1 时原有测试与新增 WAL/恢复测试合计 305 项在六配置下通过；R4-A 后扩展到 338 项；
- 新增 WAL/恢复测试通过 ASan/UBSan。

### R2：崩溃一致性与恢复模型闭环

**目标：** 把“实现了 WAL”升级为“能够证明崩溃语义”。

**任务：**

- [x] 为 WAL、快照、manifest、rename、目录 fsync 建立故障注入；
- [x] 覆盖 C01-C14 崩溃点；
- [x] 建立 ack set / possible set 恢复模型测试；
- [x] 加入随机 `kill -9` 和固定 seed 重放；
- [x] 加入 WAL/manifest parser fuzz；
- [x] 加入异常磁盘错误测试；
- [x] 将崩溃矩阵接入 CI；
- [x] 记录每个崩溃点的允许结果和不允许结果。

**验收门槛：**

- 所有已定义崩溃点通过；
- 不出现已确认写丢失；
- 不出现部分 key/value；
- 不出现旧日志复活已删除 key；
- 中间损坏不会被静默跳过；
- 恢复失败时不会发布部分 Store；
- 测试失败可凭 seed 和现场文件复现。

### R3：性能、恢复与公开证据

**目标：** 用真实数据说明持久化级别的代价，形成可公开引用的证据。

**实施结果（2026-11-11）：** 已完成 18 个配置 × 5 轮、每轮 2 秒的
durability/客户端数/负载矩阵，seed 为 `20261111`。原始数据保存为
`bench/results/reliability-20261111.json`（133,299 bytes，SHA-256
`a5c3099789be938654771f830acbd25ecedc1da5520f7c4aa1567493c0e74254`）。
18 个配置共完成 340,240 次成功请求、0 错误，采集总耗时 238.703 秒。完整汇总、
恢复与 checkpoint pause 数据见[可靠性证据](reliability-evidence.md)。

**任务：**

- [x] 扩展 KV benchmark，支持 durability mode；
- [x] 建立多客户端和混合读写负载；
- [x] 测量恢复时间与 WAL 大小；
- [x] 测量 checkpoint 停顿、RSS 和写放大；
- [x] 对比优化前后或不同模式的 P99/P999；
- [x] 生成可复现报告和原始数据；
- [x] 在 README 中只引用已验收结果；
- [x] 更新简历和面试材料模板。

**验收门槛：**

- 至少 5 轮可复现实验；
- 报告包含环境、脚本、原始输出和限制；
- 不把 closed-loop 结果写成容量结论；
- 能解释 `SYNC`、`INTERVAL` 的取舍，并说明组提交为何作为 `SYNC` 内部优化而不暴露为 `BATCHED`；
- 能回答“为什么需要 WAL”“为什么还需要 RDB checkpoint”“checkpoint 失败怎么办”。

### R4：可选进阶

只有在 R1-R3 完成并通过证据审查后，才考虑：

**R4-A：组提交**

**实施状态（2026-11-11）：** [ADR-005](decisions/005-group-commit.md) 已固化候选边界、不可妥协不变量和 G1-G7 证据门槛；[ADR-006](decisions/006-group-commit-implementation.md) 已冻结 writer/completion 线程模型、资源上限、失败语义、checkpoint barrier 和新增观测契约。实现已落地：专用 WAL writer thread 从连续 LSN 前缀形成有界 batch，一次 data fsync 覆盖整批；owner EventLoop 仍是唯一 admission、Store apply、completion 和协议响应点。HTTP/KV mutation、SAVE、checkpoint 和 shutdown 已异步化，队列/backlog 满时明确拒绝或 fail-stop。实现提交 `a06d7c2` 的 GitHub Actions 9/9 通过；正式 G4 在该 clean HEAD 上完成 5 轮 32 客户端 `SYNC` write，median QPS **2,858.989**，为冻结基线 418.018 的 **6.84x**，29,100/29,100 次成功且 0 错误，判定 PASS。G1-G7 已闭环；收益必须与更高的 CPU、略高的 server RSS 和约 2 倍恢复时间一起引用。

- [x] 写 ADR，定义候选边界、失败矩阵和证据门槛；
- [x] 增加低开销诊断指标和 `/stats`/`STATS` 契约；
- [x] 采集 clean v2 基线并判定 G1/G2：fsync 占 `SYNC` 持久化耗时 99.06%；
- [x] G3：独立非生产实验证明一次 fsync 窗口可形成至少 8 条连续待提交记录；
  5 轮、2,985 个窗口、0 错误，汇总 `>=8` 比例 0.987605；artifact
  `bench/results/r4a-batch-formation-20261111.json`；
- [x] 实现级 ADR-006：writer/completion、资源上限、失败矩阵和新增崩溃点；
- [x] 专用 WAL writer thread 和有界 admission queue；
- [x] 有界 completion backlog、合并 drain 和 callback 恰好一次；
- [x] 一次 data fsync 服务多个连续写，不增加固定 sleep；
- [x] 保持 ack 后不丢、LSN 连续性和全局 apply/completion 顺序；
- [x] HTTP/KV 异步 mutation、SAVE、checkpoint barrier 和 graceful shutdown；
- [x] 顶层 `group_commit` 指标、batch-size 桶、queue/backlog 高水位和失败计数；
- [x] G5/G6 本地验证：六配置各 338 项测试、24 个进程用例、TSan/sanitizer/fuzz/静态分析门禁；
- [x] G4：在 clean HEAD `a06d7c2` 上用相同环境、数据集和 5 轮实验验证 32 客户端 write；median QPS 2,858.989，相对 418.018 提升 6.84x，PASS；
- [x] 将 G4 artifact、限制和 digest 写回 ADR-005、ADR-006、证据文档与项目状态，完成 G7；最终证据提交以自身 `main` HEAD 的完整 CI 为发布门禁。

**R4-B：后台全量 checkpoint（已完成）**

实施状态（2026-11-11）：[ADR-007](decisions/007-background-checkpoint.md) 已冻结
两阶段 admission cut、独立 snapshot、完整 live WAL segment manifest、worker
I/O、cleanup 重试和失败语义。实现提交 `fbfd6a6` 的本地六配置各 348/348 项
CTest、25 个进程用例、TSan、sanitizer、四个 20,000-run fuzz target、格式和
静态分析通过。正式 artifact
`bench/results/r4b-background-checkpoint-20261111.json`（43,519 bytes，SHA-256
`d562809738df883e6bf9b9bf317569ab9aa66b8cfb7c72b2e5a8cbaec2ba6aa4`）采集于
clean HEAD `fbfd6a6`。主 benchmark 5 轮、32 clients `SYNC` write 完成
34,916/34,916 次请求、0 错误，median QPS 3,526.336；checkpoint probe 5 轮
SAVE 全部成功、0 failure，checkpoint total 中位数 387,509 us，serialize 中位数
331,836 us。probe 的 233 个 `ERROR 100` 是显式 backpressure，不是已确认写丢失；
指标不能证明容量、掉电一致性或任意数据集的 SAVE 延迟上界。

- [x] 比较 background snapshot、copy-on-write 和增量 checkpoint 的边界；
- [x] 采用后台全量 snapshot，保留两阶段短 admission cut；
- [x] 保证 manifest 记录完整 live segment 集合和原子发布顺序；
- [x] 覆盖 rename 后/manifest 前崩溃、cleanup 残留和 callback 生命周期；
- [x] 完成本地六配置、sanitizer、TSan、fuzz 和静态分析门禁；
- [x] 采集 clean-HEAD 正式 artifact，并把限制、digest 和分段延迟写回文档；
- 本证据提交必须在自身 `main` HEAD 上通过完整 GitHub Actions 9/9；在该提交的远程检查完成前不写成“已通过”。

**R4-C：MVCC/事务（下一阶段）**

- [ ] 先写 ADR-008，冻结版本链、隔离级别、冲突检测、事务 WAL、GC、TTL 交互、协议、资源上限、snapshot/checkpoint barrier 和 rollback 恢复；
- [ ] ADR-008 单独提交、推送并通过自身 9/9 CI 后才开始实现；
- [ ] 适合数据库内核方向，但不与 WAL/checkpoint 阶段混合。

**R4-D：AI Agent 持久状态后端**

- [ ] 如果目标偏向 AI 基础设施，可以在 WAL 之上增加：
  - 幂等键；
  - lease；
  - 版本号/CAS；
  - 会话和工具调用状态恢复；
- [ ] 这可以作为差异化应用切口，但不能替代存储核心的崩溃一致性工作。

---

## 9. 风险与应对

| 风险 | 影响 | 预防/应对 |
|---|---|---|
| WAL 格式和恢复语义不完整 | 可能出现静默丢数据 | 先写 ADR 和格式规范，再做实现 |
| fsync 阻塞 owner loop | 延迟和吞吐变差 | R1 接受正确性基线；R4 再评估组提交 |
| manifest/快照发布顺序错误 | 启动加载错误状态 | manifest 必须引用已持久化对象；覆盖崩溃点 C07-C12 |
| 重放不幂等 | 重复应用或复活删除 | LSN 单调、绝对 TTL、模型测试和幂等性验证 |
| TTL 跨重启漂移 | 旧数据复活或提前过期 | WAL 保存绝对 deadline；时钟跳变测试 |
| 磁盘满/EIO | 写入失败或日志损坏 | fail-stop、保留现场、不返回成功、故障注入 |
| WAL 无限增长 | 磁盘耗尽、恢复变慢 | segment 上限、checkpoint 触发、资源预算 |
| 两个进程访问同一目录 | 日志和快照互相破坏 | 单写者锁；文档和启动检查 |
| 旧 RDB 迁移不明确 | 启动失败或误判空库 | legacy import 规则；存在 WAL 时拒绝自动迁移 |
| 性能数据不可信 | 简历和 README 过度承诺 | 固定环境、原始数据、限制说明 |
| 崩溃测试不稳定 | CI 噪音和误判 | 确定性注入、固定 seed、artifact、分层 CI |
| 范围膨胀 | 主线被 SQL/集群/事务拖垮 | 非目标清单和阶段准入条件 |

---

## 10. 对外表达、简历与面试

### 10.1 README 开头建议

```text
MiniDB 是一个使用 C++20 实现的单机 KV 存储服务，面向有限内存、
有限连接的单节点场景，重点验证从网络请求到崩溃恢复的持久化闭环。

当前版本是可靠性工程原型，不宣称生产可用，也不宣称 Redis/SQL 兼容。
```

### 10.2 README 增加“核心问题”

```text
当客户端收到写成功响应后，进程在任意点崩溃，数据是否一定还在？
不同持久化级别下，性能与可能丢失窗口的边界是什么？
恢复过程如何区分尾部半写、日志损坏和正常日志？
```

### 10.3 README 增加“非目标”

- 不提供 SQL；
- 不追求完整 Redis 兼容；
- 不提供集群、复制或 Raft；
- 不提供事务/MVCC；
- 不提供 TLS、认证授权；
- 不宣称生产 SLA；
- 不保证多进程共享同一数据目录。

### 10.4 简历描述

R1-R3 完成后可以写：

> 设计并实现单机 crash-consistent KV 存储引擎，基于 WAL + RDB checkpoint + LSN 恢复实现已确认写不丢；构建覆盖 WAL、快照、rename、目录 fsync 和随机 kill 的崩溃注入矩阵；量化不同持久化级别的吞吐、P99 与恢复时间权衡。

在能力未完成前不要提前写入简历。可以写成计划中的目标，但不能写成已完成结果。

### 10.5 面试叙事

建议按以下顺序讲：

1. 先说明项目不是 Redis 复刻，而是可靠性工程原型；
2. 说明全量 RDB 快照为什么不能保证 ack 后不丢；
3. 说明 WAL、LSN、fsync 和 checkpoint 的分工；
4. 说明尾部半写和中间损坏的不同处理；
5. 说明 `SYNC` 与 `INTERVAL` 的语义差异；
6. 说明恢复时间和 checkpoint 停顿如何测量；
7. 主动说明当前不做的部分：SQL、集群、事务、TLS；
8. 用真实数据说明取舍，而不是只报 QPS。

可以准备的三份材料：

- 一张写路径/恢复路径图；
- 一个 WAL 尾部半写或 manifest 崩溃的最小复现；
- 一份包含原始数据和限制的持久化性能报告。

---

## 11. 阶段完成定义

本计划不以“代码写完”为完成标准，而以以下条件为准：

| 阶段 | 完成定义 |
|---|---|
| R0 | 公开入口完成新定位，核心问题、非目标、证据规范清晰 |
| R1 | `SYNC` 语义、WAL 重放、RDB checkpoint 和旧库迁移通过测试 |
| R2 | 崩溃点矩阵、恢复模型、fuzz 和 sanitizer 全部通过 |
| R3 | 持久化性能、恢复时间、checkpoint 停顿有可复现报告 |
| R4-A | 组提交只在 G1-G3 证明机会后实现，并完成 G1-G7、clean-HEAD G4 和完整门禁 |
| R4-B | 两阶段后台全量 checkpoint、完整 live segment manifest、失败恢复和正式证据通过独立门禁 |
| R4-C | 先冻结事务/MVCC ADR，再实现版本链、冲突、事务 WAL、GC、TTL 和恢复；独立提交与 CI |
| R4-D | 先冻结 Agent 状态后端 ADR，再实现幂等键、lease、CAS、会话恢复和有界资源；独立提交与 CI |

任何阶段如果出现：

- 已确认写丢失；
- 静默跳过损坏日志；
- 恢复后出现部分值；
- WAL 清理早于 manifest 发布；
- TTL 跨重启语义错误；
- 无界内存或磁盘增长；
- 新的 sanitizer/UB 问题；

都必须冻结后续功能，先修复正确性。

---

## 12. 已定稿决策与后续准入

R1 开始前的文档决策均已落地：

1. `ADR-002` 固化 WAL 记录格式、manifest 和 checkpoint 发布顺序；
2. `ADR-003` 固化 `SYNC`、`INTERVAL` 语义与默认 `SYNC`；
3. `ADR-004` 固化 segment 轮转、资源上限和清理策略；
4. `docs/wal-format.md` 给出字节级格式和非法输入矩阵；
5. `docs/recovery.md` 定义启动恢复、尾部截断和错误策略；
6. `docs/crash-consistency.md` 定义不变量、C01-C14 崩溃点和验收矩阵；
7. 保持 `-r <rdb_path>`，sidecar 为 `.manifest`、`.wal/`、`.lock`；
8. R1 已引入单写者锁以及 HTTP/KV 共享的持久化统计。

R4-A 已完成 G1-G7，不再处于待验证状态。ADR-005 的证据门槛与 ADR-006 的
实现约束均已闭环；clean-HEAD G4 artifact 记录了 median QPS 2,858.989、
相对基线 6.84x、0 错误以及 CPU/RSS/恢复时间代价。最终证据提交仍必须以自身
`main` HEAD 的完整 GitHub Actions 作为发布门禁，不能继承实现提交的结果。

R4-B 已完成并保持 R4-A 的 manifest 原子发布、ack 后不丢、全局顺序和有界资源。
R4-C/R4-D 仍是独立阶段，不随 R4-B 自动进入实现；每个阶段必须先提交并验证 ADR，
再开始代码。任何后续变更仍必须重新通过 sanitizer、崩溃矩阵、fuzz 和资源上限验证。

---

## 13. 变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-11-10 | 0.1 | 初稿：完成定位调整、WAL/checkpoint 设计、崩溃测试、性能和 R0-R4 路线规划 |
| 2026-11-11 | 0.2 | R0-R3 完成实现与本地验收：305 项测试、四个 fuzz target、18 配置可靠性 benchmark；R4 未启动 |
| 2026-11-11 | 0.3 | R4-A 完成 ADR-005、timing 指标和 clean v2 baseline；G1/G2 PASS，G3 待验证；组提交实现仍未开始 |
| 2026-11-11 | 0.4 | R4-A 完成独立非生产 G3 batch formation 实验：5 轮、2,985 个窗口、0 错误，汇总 `>=8` 比例 0.987605，G3 PASS；等待实现级 ADR，组提交仍未实现 |
| 2026-11-11 | 0.5 | R4-A 实现完成：ADR-006、writer/queue/batch/completion、异步 HTTP/KV、SAVE/checkpoint/shutdown barrier、`group_commit` 指标和本地六配置 338 项测试；G1-G3 PASS、G5/G6 本地 PASS、G7 已记录，G4 待 clean HEAD 采集 |
| 2026-11-11 | 0.6 | R4-A G4 PASS：clean HEAD `a06d7c2`、5 轮 32 客户端 `SYNC` write、median QPS 2,858.989、相对 418.018 为 6.84x、29,100 次成功 0 错误；artifact、digest、CPU/RSS/恢复代价和 G7 状态已写回，R4-A 完成 |
| 2026-11-11 | 0.7 | R4-B 完成：ADR-007、两阶段后台全量 checkpoint、完整 live segment manifest、cleanup 重试、348/25 本地门禁和 clean-HEAD v3 artifact；下一步先提交 ADR-008，再实现 R4-C |
