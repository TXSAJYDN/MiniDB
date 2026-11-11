# ADR-008：R4-C MVCC 与快照隔离事务

- 状态：Proposed — 仅冻结设计；实现尚未开始，必须在本 ADR 单独提交并取得自身 9/9 CI 后才能落地
- 日期：2026-11-11
- 关联：`docs/MiniDB-可靠性与持久化升级计划书.md`、ADR-002、ADR-003、ADR-004、ADR-006、ADR-007、`docs/wal-format.md`、`docs/recovery.md`、`docs/protocol-kv.md`
- 决策范围：R4-C 的版本链、隔离级别、冲突检测、事务 WAL、协议、资源上限、GC、TTL、checkpoint 与恢复语义

## 背景

当前 `KvStore` 是单个 `std::mutex` 保护的
`unordered_map<std::string, InternalEntry>`。一次 standalone SET/DEL/EXPIRE
在 WAL 记录持久化后按 LSN 顺序应用到唯一当前值。这个模型足以支撑教学和可靠性
原型，但不能表达“多个操作作为一个原子状态转换”、多个客户端读快照和写冲突。

R4-C 的目标是让 MiniDB 真正拥有一个可验证的事务内核边界，同时保持：

- 已确认写不丢和 WAL-before-apply；
- owner EventLoop 是唯一 admission/apply/completion 点；
- 现有 RDB、manifest、WAL segment 和 checkpoint 发布顺序；
- 有界内存、WAL、版本数、事务数和协议帧；
- 进程崩溃、WAL 尾部半写和恢复时不出现部分事务。

R4-C 不是复制 PostgreSQL/Redis，也不提供分布式事务、串行化隔离、范围查询、
持久化游标、跨进程事务或生产 SLA。目标是单进程、单 owner、多连接下可解释的
**快照隔离（Snapshot Isolation, SI）**。

## 决策摘要

1. 采用 **first-committer-wins 的快照隔离**。事务读取 BEGIN 时最后已应用的
   LSN 视图，写写冲突在 COMMIT admission 时检测。
2. `KvStore` 从单版本 map 改为“key -> 按 commit LSN 递增的版本链”。版本只在
   已提交 WAL 记录应用后发布；事务私有写集不进入 live store。
3. 事务在 COMMIT 时编码为 **一条 WAL v2 事务记录**，获得一个 LSN。该记录是
   原子提交单元；恢复时整条记录全部应用或全部忽略，不产生部分事务。
4. WAL v2 保持 v1 记录可读；v1 记录继续按单操作事务重放。升级后的写入使用
   v2。不支持把含 v2 记录的数据库降级给旧二进制。
5. 显式事务只在二进制 KV 协议中开放。HTTP `/kv` 的每个 POST/DELETE 是隐式
   单操作事务；HTTP 不提供跨请求事务句柄，该限制写入协议文档。
6. 历史版本只服务活跃快照。GC 保留每个活跃 snapshot 所需的最近版本及最新版本；
   若无法在硬上限内保留，则拒绝新事务或新写，而不是悄悄破坏旧快照。

## 不变量

1. 一个 WAL 事务记录对应唯一 LSN；同一事务的多个 key 更新不会拆成多条 WAL。
2. 已确认 COMMIT 的所有 key 更新在恢复后同时存在；未确认/未写入 COMMIT 的
   事务不产生可见更新。
3. 任何 key 的版本链按 `commit_lsn` 严格递增，同一 LSN 内每个 key 最多一个
   最终写操作。
4. 事务只能读取 `commit_lsn <= snapshot_lsn` 的版本和自己的写集；不能读脏写。
5. 两个事务写同一 key 时，后 COMMIT 者只有在先提交发生在它的 snapshot 之后时
   才冲突；冲突结果固定为 `CONFLICT`，不静默覆盖。
6. active transaction、版本、写集、WAL batch 和协议帧均有硬上限。
7. checkpoint 只捕获已提交且已 apply 到 `checkpoint_lsn` 的折叠状态；不持久化
   活跃事务的私有写集。
8. TTL deadline 继续以绝对 Unix ms 进入 WAL/RDB；版本可见性与 TTL 过期判定
   分离，过期不会复活旧版本。
9. 现有“没有成功 fsync 就不能承诺 SYNC 成功”和 callback 恰好一次语义不变。
10. 不需要也不允许修改 `LICENSE` 或 `COPYING`。

## 1. 隔离级别与可见性

### 1.1 固定为快照隔离

BEGIN 在 owner 线程记录：

```text
snapshot_lsn = 当前已经 apply 的最大 LSN
```

读取规则，按优先级：

1. 如果 key 在事务私有写集中，返回私有写集的状态（read-your-writes）；
2. 否则在 live 版本链中寻找 `commit_lsn <= snapshot_lsn` 的最大版本；
3. 找不到版本或版本是墓碑，返回 missing；
4. 找到值版本后，再用当前单调时钟判断 expiry。已过期返回 missing；不回退到
   更旧版本。

由此提供：

- 无脏读；
- 可重复读：同一事务对未写 key 的读取不受 BEGIN 后其他提交影响；
- read-your-writes；
- 写写冲突 first-committer-wins；
- 允许 write skew，因为不提供 predicate/range conflict。

不承诺串行化（serializable）。文档必须明确 SI 的 write skew 限制，不能用
“事务”暗示可序列化。

### 1.2 LSN 与提交序列

LSN 仍是唯一全局提交序列。standalone SET/DEL/EXPIRE 是只有一个操作的隐式
事务，各占一个 LSN；显式事务提交占一个 LSN。一个 LSN 内可以包含多个 key。
不存在“内存 commit sequence 与 WAL LSN 两套顺序”。

## 2. KvStore 版本链改造

### 2.1 数据结构

当前结构：

```cpp
struct InternalEntry {
    std::string value;
    steady_clock::time_point deadline;
    bool has_ttl = false;
};
std::unordered_map<std::string, InternalEntry> store_;
```

改为：

```cpp
struct Version {
    std::uint64_t commit_lsn = 0;
    std::string value;
    std::chrono::steady_clock::time_point deadline;
    bool has_ttl = false;
    bool deleted = false;
};

using VersionChain = std::vector<Version>; // commit_lsn 升序
std::unordered_map<std::string, VersionChain> versions_;
```

版本链仍由 `KvStore::mutex_` 保护。事务私有写集由 owner 线程持有，不进入
KvStore map，也不参与 scanner 遍历。TTL scanner 只能回收已经过期且被 GC 判定
可删除的物理版本，不产生 WAL。

### 2.2 原子 apply

新增语义等价的内部入口：

```cpp
struct AppliedMutation {
    enum class Kind { kSet, kDelete, kExpire } kind;
    std::string key;
    std::string value;
    std::int64_t absolute_expiry_ms = 0;
};

bool apply_committed(std::uint64_t commit_lsn,
                     const std::vector<AppliedMutation>& mutations,
                     std::string* error);
```

要求：

- 调用者保证 `commit_lsn > 当前最大已 apply LSN`；
- 先完整校验 key/value/expiry 和同一 key 的最终状态，再一次持锁把每个 key 的
  最终版本追加到一个 commit LSN；
- 对同一事务内的同 key 多次操作先折叠为一个最终操作：SET 可带 absolute
  expiry，DEL 为墓碑，EXPIRE 只改变 deadline；
- 版本追加完成后才更新 manager 的 `last_lsn_`；
- 内存分配/apply 异常视为不可继续的 fail-stop，必须要求重启；WAL 中的完整
  事务记录仍可在恢复时重新原子应用。

旧 `set/get/del/expire/ttl_info/export_all/restore_entry` 继续保留给现有测试和
in-memory 模式，但它们必须通过内部合成版本实现，不能绕过版本链。服务器持久化
路径使用带 LSN 的 apply 接口。

### 2.3 读取接口

```cpp
std::optional<std::string> get_at(const std::string& key,
                                  std::uint64_t snapshot_lsn);
TtlResult ttl_info_at(const std::string& key, std::uint64_t snapshot_lsn);
```

`export_all()` 在 checkpoint 边界只输出每个 key 当前最新的已提交 live 状态，
自然折叠版本链；RDB v2 格式不变。

## 3. 事务对象与 owner 线程边界

`PersistenceManager` 拥有唯一的 `TransactionTable`，所有 BEGIN/读写/COMMIT/
ROLLBACK 状态转换都在 owner EventLoop 执行。KvSession 只保存当前事务 id，
不持有可被其他线程访问的版本链指针。

```cpp
struct TransactionState {
    std::uint64_t txn_id;
    std::uint64_t snapshot_lsn;
    std::uint64_t started_us;
    std::unordered_map<std::string, PendingWrite> writes;
    std::uint64_t write_bytes;
    State state; // active, committing, aborted
};
```

状态机：

```text
BEGIN -> Active
Active --COMMIT accepted--> Committing --wal/apply ok--> Committed
Active --COMMIT conflict--> Conflicted (rollback)
Active --ROLLBACK/close--> RolledBack
Committing --persistence failure--> Failed (fail-stop)
```

一个连接最多一个 active/committing 事务。COMMIT 发出后，该会话不解析下一条
命令，直到完成回调；其他连接可以继续。连接断开、协议错误或 session 析构时必须
回滚其未提交事务；已经受理的 COMMIT 仍由 PersistenceManager 完成并恰好回调
一次，回调发现 session 已销毁时 no-op。

事务 id 为进程内单调 `uint64_t`，从 1 开始；溢出返回错误并停止创建事务。
事务 id 不持久化，也不跨重启复用其含义。

## 4. 冲突检测

### 4.1 first-committer-wins

事务在 BEGIN 时记录 `snapshot_lsn`。COMMIT admission 对写集中的每个 key 检查：

```text
max(committed_key_lsn, pending_key_lsn) > snapshot_lsn  => conflict
```

这里：

- `committed_key_lsn` 是最后一次已 apply 到该 key 的 LSN；
- `pending_key_lsn` 是已经受理但尚未 apply 的 standalone/事务记录对该 key 的
  最大 LSN；
- 本事务尚未分配的 commit LSN 不参与检查。

必须覆盖“前一个事务已通过 admission 但还在组提交队列中”的窗口。不能只看 live
store，否则两个并发 COMMIT 可能都通过检查并互相覆盖。

### 4.2 结果与重试

- 无冲突：分配一个 LSN，编码单条事务 WAL，进入组提交；
- 冲突：不分配 LSN，不写 WAL，事务立即结束并返回 `CONFLICT`；
- 资源上限/参数非法：不分配 LSN，事务结束（客户端需要重新 BEGIN）；
- WAL 或 apply 失败：持久化层 fail-stop，事务返回错误，进程不得继续接受写。

冲突是正常并发结果，不是 I/O 错误，也不得进入 degraded 状态。

## 5. 事务 WAL v2

### 5.1 版本和兼容性

WAL v2 继续使用现有 34 字节固定 header 和 CRC32C：

```text
"MWAL" | version:u16=2 | flags:u16=0 | lsn:u64 | op:u8
       | reserved:u8=0 | key_len:u32 | value_len:u32 | expiry_ms:i64
       | key | value | crc32c:u32
```

- v2 的 standalone op 1/2/3 与 v1 完全同布局；
- v2 新增 `kTxn = 4`；
- decoder 同时接受 v1 和 v2；v1 不允许 op 4；
- encoder 对所有新记录写 version=2；
- v1 与 v2 记录可以同时存在于不同 segment；按记录版本分别解码；
- v2 kTxn 的 outer key 固定为 ASCII `txn`，outer expiry_ms=0；
- 含 v2 记录的 WAL 不允许被旧版本程序读取；降级不在支持范围。

### 5.2 kTxn value payload

outer `value` 是事务 payload，字节布局：

```text
"MTXN" | payload_version:u16=1 | op_count:u16
repeat op_count:
  op:u8                 (SET=1, DEL=2, EXPIRE=3)
  reserved:u8=0
  key_len:u16
  value_len:u32
  expiry_ms:i64
  key bytes
  value bytes
```

规则：

- 1 <= op_count <= 256；
- key 1..1024 bytes，value 0..1 MiB；
- 同一事务内同 key 已在编码前折叠，解码后不得出现重复 key；
- outer value（整个 payload）不能超过 1 MiB；
- SET 可带 0 或正绝对 expiry；DEL 的 value_len=0、expiry=0；EXPIRE 的
  value_len=0、expiry>0；
- payload opacity 由外层的 WAL CRC 覆盖；decoder 仍必须逐字段拒绝越界、重复
  key、op_count 不符、尾部多余字节和非规范保留位；
- 事务 payload 的 wire size 计入 WAL record 上限、group-commit queue 和 batch
  上限。

选择单记录而不是 begin/data/commit 多记录的原因是：

- 原子提交边界缩成一个 CRC 保护的记录；
- 恢复不需要处理“有 data 无 commit marker”的半事务；
- 不改变 GroupCommitWriter 的连续 LSN 前缀模型；
- 代价是事务总 payload 被限制为 1 MiB，这是 R4-C 明确接受的边界。

### 5.3 Standalone 与事务的一致性

standalone SET/DEL/EXPIRE 使用 v2 的 op 1/2/3，一条记录即一个隐式事务。
恢复器对 op 1/2/3 直接 apply；对 op 4 先完整解码 payload，再一次性 apply。
任何 op 4 解码错误都按中间 WAL 损坏处理，启动失败，不能跳过该事务继续重放。
## 6. 提交写路径

```text
COMMIT(owner)
  -> conflict check against committed + pending key LSN
  -> encode one WalRecord{kTxn}
  -> reserve version capacity
  -> assign LSN and enqueue GroupCommitWriter
  -> WAL append (+ data fsync for SYNC)
  -> owner receives completion in LSN order
  -> apply_committed(lsn, folded mutations) atomically
  -> publish key versions and clear pending reservations
  -> COMMIT response
```

- `SYNC`：COMMIT 成功响应不得早于覆盖该 LSN 的 data fsync；
- `INTERVAL`：append 完成后 apply/ack，仍受现有 flush/checkpoint/shutdown 驱动；
- 事务记录与 standalone 记录可在同一个物理 batch 中共享一次 fsync；
- completion 仍按 LSN 全局顺序 apply，不允许事务越过前面的 standalone 记录读取；
- 事务提交后的 GC 在 owner 线程触发，不能阻塞 writer thread。

事务提交在 WAL 已 fsync、owner apply 之前崩溃时，重启会看到完整事务。这是现有
“未确认写可能恢复出现”的允许集合，不是 ack 后丢失。SYNC 已返回成功的 COMMIT
必须恢复出全部 key。

## 7. 协议

### 7.1 二进制 KV 命令

新增 command code：

| code | 命令 | 参数 | 成功响应 |
|---:|---|---|---|
| 9 | BEGIN | 0 | `OK` + ASCII txn id |
| 10 | COMMIT | 0 | `OK` + ASCII commit LSN（只读事务为 `0`） |
| 11 | ROLLBACK | 0 | `OK` |

新增 response code：

| code | 响应 | 含义 |
|---:|---|---|
| 105 | CONFLICT | 写写冲突；事务已结束并回滚 |

已存在 SET/GET/DEL/EXPIRE/TTL 在事务 active 时自动作用于事务写集和
`snapshot_lsn`；没有 active 事务时保持现有 autocommit 语义。PING/SAVE/STATS
不改变事务状态。BEGIN/COMMIT/ROLLBACK 的参数错误、无事务 COMMIT/ROLLBACK、
重复 BEGIN 返回 `ERROR`，连接保持可用（协议帧错误仍按现有规则关闭连接）。

事务内 EXPIRE 的非正 TTL 折叠为 DEL；SET 清除 TTL；TTL 返回事务视图的结果。
READ-only 事务 COMMIT 不写 WAL，直接成功，响应 LSN 为 0。

### 7.2 HTTP 边界

HTTP 的 `GET /kv/<key>` 读取最新已提交状态；`POST /kv/<key>` 和
`DELETE /kv/<key>` 是单操作隐式事务。R4-C 不增加 HTTP 跨请求事务 token，
因为当前 HTTP 层没有会话级事务资源模型；在文档中明确这一限制，并仍通过
`/stats` 暴露事务/版本指标。未来增加 HTTP 事务需要单独协议 ADR，不能描述为
已经支持。

## 8. TTL、GC 与版本上限

### 8.1 TTL

- 版本保存 monotonic deadline 和绝对 Unix expiry；
- 读取时先按 snapshot 选版本，再判断当前 deadline；
- 过期版本不回退到更旧版本；
- TTL scanner 的物理清理不产生 WAL、不改变任何活跃 snapshot 的逻辑结果；
- 事务内 TTL 基于事务视图计算，COMMIT 时写最终 absolute deadline；
- RDB/WAL 恢复不重新起算相对 TTL。

### 8.2 GC

维护活跃事务的 `snapshot_lsn` 集合，取最小值 S。对每个 key：

- 保留最新版本；
- 对每个活跃 snapshot，保留 `commit_lsn <= snapshot` 的最新版本；
- 其余中间版本可删除；
- 若最新版本是墓碑且所有活跃 snapshot 都 >= 该墓碑 LSN，可删除整个 key；
- GC 只在 owner 线程的提交/回滚/显式维护点执行，绝不与版本读取并发破坏快照。

没有活跃事务时可以只保留每个 key 的最新版本。GC 失败不能破坏 live map；如果
无法满足硬上限，新的 BEGIN 或写 admission 返回 `kResourceLimit`。

### 8.3 硬上限

| 资源 | 上限 |
|---|---:|
| 全局 active transactions | 64 |
| 单事务写 key 数 | 256 |
| 单事务 payload | 1 MiB |
| 单 key | 1 KiB |
| 单 value | 1 MiB |
| 全局版本条目 | 1,000,000 |
| 单 WAL record | 现有 1 MiB + header/CRC 上限 |
| 事务 WAL op_count | 256 |

上限必须可通过测试注入缩小；生产默认值写入文档和 stats。超过上限的 COMMIT
在分配 LSN 前失败，不能先写 WAL 再报告资源错误。

## 9. Checkpoint、恢复与 rollback

### 9.1 checkpoint barrier

R4-B 的两阶段 pause/drain/rotate 保持不变。checkpoint 的 capture 只导出：

- `commit_lsn <= checkpoint_lsn` 的每个 key 最新已提交版本；
- 绝对 TTL；
- 已提交墓碑不进入 RDB；
- active transaction 私有写集不进入 RDB，也不产生 WAL record。

checkpoint 不取消活跃事务；它们可以继续，但其未提交写永远不会被 manifest 覆盖。
如果 COMMIT 在第一次 barrier 前已被接受，writer drain 必须让它完成 apply 后才
capture；如果 COMMIT 在 capture 后受理，则进入 `checkpoint_lsn + 1` 之后的
WAL，恢复时整体重放。

### 9.2 恢复

`RecoveryManager` 继续从 manifest 的 `checkpoint_lsn + 1` 重放。遇到：

- v1 op 1/2/3：按现有单操作语义应用；
- v2 op 1/2/3：同上；
- v2 op 4：完整校验 payload，构造折叠 mutations，在临时 store 上一次
  `apply_committed(lsn, ops)`；任一 op 失败则整个恢复失败；
- 事务记录被 WAL 尾部截断：只接受“记录未完整”导致的尾部截断；完整 CRC 失败
  或 payload 非法属于损坏，必须 fail closed；
- 重复 key、未知 op、payload 长度不一致：启动失败，不发布部分恢复结果。

rollback 不写 WAL，因此 rollback 后崩溃不会产生数据库状态变化。未收到提交
请求的事务也没有 WAL。已写入并被 fsync 但没有返回成功的 COMMIT 允许在重启后
出现，符合现有 possible-set 语义。

### 9.3 shutdown

shutdown 先拒绝新事务/外部 mutation，再等待已受理的 COMMIT 和 standalone 写
完成 apply，最后执行现有 checkpoint。未提交 active transactions 被明确 rollback；
不能把它们的私有写集写入 RDB，也不能因为 shutdown 而伪造 COMMIT。

## 10. 失败矩阵

| 场景 | 事务结果 | 持久化结果 | 重启后 |
|---|---|---|---|
| BEGIN 超过 active 上限 | ERROR | 无 | 无变化 |
| 写集超过 256 key/1 MiB | ERROR，事务结束 | 无 | 无变化 |
| COMMIT 写写冲突 | CONFLICT，事务结束 | 无 | 无变化 |
| COMMIT 编码/资源检查失败 | ERROR，事务结束 | 无 | 无变化 |
| WAL append 失败 | ERROR，fail-stop | 不确认 | 可能无记录或尾部可截断 |
| WAL fsync 失败（SYNC） | ERROR，fail-stop | 不确认 | 记录可能完整出现，不报告成功 |
| WAL 完整、apply 前崩溃 | 未确认 | 记录可恢复 | 完整事务出现 |
| apply 中进程崩溃 | 未确认 | 完整记录已在 WAL | 恢复原子重放全部 |
| COMMIT 成功响应后崩溃 | 成功 | 已 fsync（SYNC） | 全部 key 可见 |
| ROLLBACK 后崩溃 | 成功回滚 | 无记录 | 无变化 |
| 连接断开 | 自动 rollback | 无记录 | 无变化 |
| checkpoint 与 active txn 并发 | txn 继续 | RDB 不含私有写集 | 事务丢失，已提交数据一致 |
| WAL txn payload 损坏 | 启动失败 | 保留现场 | 不发布部分 DB |

## 11. 观测契约

在现有 `/stats` 和 KV `STATS` 的顶层增加 `transactions` 对象；所有字段来自
同一个 `PersistenceSnapshot` 锁区间：

| 字段 | 含义 |
|---|---|
| `active` | 当前 active/committing 事务数 |
| `started` | 成功 BEGIN 数 |
| `committed` | 成功 COMMIT 数（含只读） |
| `conflicted` | 返回 CONFLICT 的事务数 |
| `rolled_back` | 显式/断连 rollback 数 |
| `failed` | 因资源/持久化错误结束的事务数 |
| `write_conflicts` | 冲突 key 检查次数 |
| `max_active` | active 高水位 |
| `versions_current` | 当前版本条目数 |
| `versions_max` | 版本条目高水位 |
| `versions_reclaimed` | GC 删除版本数 |
| `oldest_snapshot_lag_lsn` | 当前最老 snapshot 与 last_lsn 的差 |
| `max_oldest_snapshot_lag_lsn` | 上述差值高水位 |
| `commit_records` | 成功提交的事务 WAL record 数 |
| `commit_bytes` | 这些 record 的 wire bytes 总和 |
| `commit_total_us` / `commit_max_us` | COMMIT admission 到 apply/ack 的耗时 |
| `gc_total_us` / `gc_max_us` | GC 累计/最大耗时 |

指标是诊断数据，不是 SLA；没有任意分位数。字段名和嵌套位置一旦进入实现，
必须由 JSON 契约测试锁定。
## 12. 代码落点

- `src/storage/KvStore.{h,cpp}`：版本链、snapshot read、原子
  `apply_committed`、折叠 export、TTL 与 GC；
- `src/storage/WalFormat.{h,cpp}`：WAL v2、kTxn payload codec、v1 兼容解码；
- `src/storage/WalReplay.{h,cpp}`：事务记录的原子 apply 和非法 payload 拒绝；
- `src/storage/PersistenceManager.{h,cpp}`：TransactionTable、冲突检测、
  pending key LSN、commit async、stats、shutdown/GC；
- `src/storage/GroupCommitWriter.{h,cpp}`：只复用现有连续 LSN/批提交路径，
  不引入事务线程；
- `src/protocol/KvProtocol.{h,cpp}`、`src/protocol/KvSession.{h,cpp}`：
  BEGIN/COMMIT/ROLLBACK、事务内命令路由、CONFLICT 和断连 rollback；
- `src/protocol/PersistenceStatsJson.h`：`transactions` 对象；
- `docs/protocol-kv.md`、`docs/wal-format.md`、`docs/recovery.md`、
  `docs/crash-consistency.md`：协议、格式、崩溃点和限制；
- `tests/`：隔离、冲突、WAL、恢复、checkpoint、TTL、GC、资源、协议和 fuzz。

## 13. 验证要求

实现提交必须覆盖并记录：

1. 无脏读、可重复读、read-your-writes、只读事务；
2. 两个并发事务写同一 key 时恰有一个成功；
3. pending commit 与 live store 冲突检测窗口；
4. 事务内 SET/DEL/EXPIRE/TTL 的折叠和过期；
5. COMMIT 单条 WAL 记录的编码/解码/fuzz，v1/v2 混读；
6. WAL 尾部半写、payload 损坏、重复 key、未知 op 的失败矩阵；
7. 完整 WAL、apply 前崩溃、apply 后崩溃的恢复原子性；
8. rollback 后无 WAL 且重启无变化；
9. active snapshot 的 GC 不提前删除所需版本；
10. checkpoint 忽略未提交事务，并正确保留 capture 后的已提交事务 WAL；
11. 六配置 CTest、ASan/UBSan/ASan+UBSan/TSan、格式、静态分析和四个 fuzz
    target；
12. `/stats`/`STATS` 的事务字段契约测试。

在任何阶段，如果出现 ack 后事务部分丢失、恢复出现部分 key、GC 破坏快照、
WAL 记录被静默跳过或无界版本增长，立即冻结 R4-C，不允许继续 R4-D。

## 14. 已考虑的替代方案

### 只做单条命令的事务包装

实现成本最低，但不产生多 key 原子性和版本快照，无法回应 R4-C 的核心问题。
拒绝。

### 多记录 begin/data/commit

支持超过 1 MiB 的事务，但需要 begin/commit marker、未提交记录隔离、崩溃恢复
扫描和 WAL 保留边界的新状态机。R4-C 选择先冻结单记录原子单元；未来若要扩展，
必须升级 WAL version 并另写 ADR。

### 可序列化隔离

需要 predicate/range conflict 或 SSI，当前没有范围扫描，且验证成本高。先交付
明确声明的 SI；write skew 是已知限制，不是隐藏承诺。

### 在 live map 上原地修改后异步写 WAL

会违反 WAL-before-apply，并让回滚和崩溃出现部分状态。拒绝。

## 15. 后果

正面：

- 真正拥有多 key 原子提交和快照读；
- 事务复用现有组提交、WAL、checkpoint 和恢复边界；
- v1 WAL 可读，升级不需要重写历史段；
- 版本/事务/写集上限可测量、可注入、可失败；
- 为 R4-D 的 CAS、lease 和 session state 提供可用的版本基础。

代价与限制：

- 事务 payload 受 1 MiB 硬上限；
- SI 允许 write skew，不提供 serializable；
- 活跃快照会保留历史版本，长时间/大量事务可能触发资源拒绝；
- HTTP 仍无跨请求事务；
- 事务记录是一条 WAL 记录，单事务不能超过现有 WAL record/batch 边界；
- 版本链和 GC 增加内存与复杂度，必须用 TSan、fuzz 和恢复矩阵验证，不能
  宣称生产级并发或容量。

## 16. 实施门槛

本 ADR 单独提交、推送 `main` 并通过该提交自身的完整 GitHub Actions 9/9 前，
不得开始修改实现代码。实现开始后如果本 ADR 的格式、隔离级别或失败语义发生
变化，必须更新 ADR 并重新经过同等 CI/证据门禁。