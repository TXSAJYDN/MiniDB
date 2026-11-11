# WAL 与 checkpoint manifest 格式

本文描述 MiniDB 的 WAL v2、WAL v1 兼容读取和 checkpoint manifest v1 的字节级
布局。所有多字节整数均为**大端序（big-endian）**，CRC 使用 CRC32C（Castagnoli
反射多项式）。WAL v2 在 v1 的外层记录布局上增加了显式事务记录 `kTxn`，编码器
始终写 v2，解码器同时接受 v1 和 v2。

## 持久化目录布局

给定 `-r <rdb_path>`：

~~~text
<rdb_path>              RDB v2 checkpoint snapshot
<rdb_path>.manifest     checkpoint manifest v1
<rdb_path>.wal/         WAL segment 目录
<rdb_path>.lock         flock 单写者锁文件
~~~

示例：

~~~text
/var/lib/minidb/minidb.rdb
/var/lib/minidb/minidb.rdb.manifest
/var/lib/minidb/minidb.rdb.wal/
/var/lib/minidb/minidb.rdb.lock
~~~

WAL segment 文件名固定为 20 位十进制起始 LSN：

~~~text
wal-00000000000000000001.log
wal-00000000000000001001.log
~~~

目录中的未知文件不会被当作 WAL 输入；名称合法但元数据越界的 segment 会导致
启动失败。

## WAL 外层记录

### 通用布局

v1 和 v2 使用相同的外层头：

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `MWAL` |
| 4 | 2 | version | `u16`，`1` 或 `2` |
| 6 | 2 | flags | `u16`，所有已定义版本必须为 0 |
| 8 | 8 | lsn | `u64`，严格大于 0 |
| 16 | 1 | op | v1 为 `SET/DEL/EXPIRE`；v2 另允许 `TXN` |
| 17 | 1 | reserved | 必须为 0 |
| 18 | 4 | key_len | `u32`，1..1024 |
| 22 | 4 | value_len | `u32`，0..1,048,576 |
| 26 | 8 | expiry_ms | `i64`，绝对 Unix 毫秒；0 表示无过期 |
| 34 | key_len | key | 原始字节 |
| 34+key_len | value_len | value | 原始字节 |
| 末尾 4 | 4 | crc32c | 覆盖本记录除末尾 CRC 外的全部字节 |

固定头部为 34 字节，校验和为 4 字节：

~~~text
record_size = 34 + key_len + value_len + 4
~~~

最大外层记录为 `34 + 1024 + 1,048,576 + 4 = 1,049,638` 字节。事务记录的
外层 key 固定为 `txn`，因此其最大记录略小于该理论上限。

### 操作码

| op | 名称 | v1 | v2 | value | expiry_ms | 语义 |
|---|---|---:|---:|---:|---:|---|
| `1` | `SET` | 是 | 是 | 0..1 MiB | 0 或正绝对截止时间 | 设置 key；命令路径写 0，表示清除 TTL |
| `2` | `DEL` | 是 | 是 | 0 | 0 | 删除 key |
| `3` | `EXPIRE` | 是 | 是 | 0 | 必须 > 0 | 设置绝对 TTL deadline |
| `4` | `TXN` | 否 | 是 | MTXN payload | 0 | 原子应用一组已提交 mutation |

v1 解码时最大合法 op 为 `EXPIRE=3`，所以任何 v1 `TXN` 记录都被拒绝。v2
standalone `SET/DEL/EXPIRE` 与 v1 的同名记录布局和语义完全相同。历史 v1
记录不需要重写，可以与 v2 记录出现在同一 segment 或不同 segment 中。

`SET` 在格式上允许正 expiry，以便恢复层保留绝对 deadline；当前单操作命令
路径的 SET 会清除 TTL，因此实际写入的 expiry 为 0。

### 编码与解码状态

- `kOk`：完整记录通过外层 CRC、字段校验和事务 payload 校验，返回消费字节数；
- `kNeedMore`：输入是截断前缀，只有恢复层的最后一个 segment 可以把它当作尾部；
- `kCorrupt`：头部、CRC、操作字段或完整事务 payload 非法，恢复拒绝启动。

解码器不会跳过损坏记录，也不会在 CRC 失败后重新同步。

## kTxn / MTXN 事务 payload

### 布局

`TXN=4` 的 value 是一个完整的 MTXN payload：

~~~text
"MTXN" | payload_version:u16=1 | op_count:u16
repeat op_count times:
  op:u8 | reserved:u8=0 | key_len:u16 | value_len:u32
  expiry_ms:i64 | key | value
~~~

MTXN payload 头 8 字节，每个操作头 16 字节。payload 的总大小是：

~~~text
8 + sum(16 + key_len + value_len)  [每个操作]
~~~

### 操作约束

| op | value_len | expiry_ms | 语义 |
|---|---:|---:|---|
| `SET=1` | 0..1 MiB | 0 或正绝对截止时间 | 设置 key；同一次事务内不得重复出现同一 key |
| `DEL=2` | 0 | 0 | 删除 key；不得与其他同 key 操作重复 |
| `EXPIRE=3` | 0 | 必须 > 0 | 设置绝对 TTL deadline；不得与其他同 key 操作重复 |

事务内操作保持编码顺序。提交层必须先把同一 key 的多次写折叠为最终状态，再
编码 MTXN，因此合法 COMMIT 不会产生重复 key。解码器仍会拒绝重复 key、嵌套
`TXN`、未知 op、非零 reserved、非法 DEL/EXPIRE 以及尾部冗余字节，防止损坏或
恶意输入绕过不变量。

### 资源边界

| 边界 | 值 |
|---|---:|
| 单事务操作数 | 1..256 |
| MTXN payload | 1..1,048,576 字节 |
| 单操作 key | 1..1024 字节 |
| 单操作 value | 0..1,048,576 字节 |
| 事务外层 key | 必须为 `txn` |
| 事务外层 expiry | 必须为 0 |

MTXN payload 达到 1 MiB 是允许的；超过 1 MiB、op_count 为 0 或大于 256 均
拒绝。外层记录 CRC 覆盖完整 MTXN payload。

### 原子恢复语义

恢复层先完整解码并校验一条 MTXN，再把全部操作转换为 `AppliedMutation` 并调用
一次 `KvStore::apply_committed_status`。事务记录只占一个 WAL LSN，只推进一次
`current_lsn`，成功后所有 key 的 `latest_committed_lsn` 都等于该 LSN。校验失败、
资源上限或 apply 失败时，一个 key 都不会发布；恢复不会保留事务前缀操作。

## Checkpoint manifest v1

### 布局

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `MMAN` |
| 4 | 2 | version | `u16`，必须为 1 |
| 6 | 2 | flags | `u16`，v1 必须为 0 |
| 8 | 8 | checkpoint_lsn | `u64`，快照覆盖到的最后 LSN |
| 16 | 4 | retained_count | `u32`，0..128 |
| 20 | 4 | reserved | 必须为 0 |
| 24 | 8 * count | start_lsn[] | 严格递增的 segment 起始 LSN |
| 末尾 4 | 4 | crc32c | 覆盖前面全部字节 |

固定头部为 24 字节，校验和为 4 字节：

~~~text
manifest_size = 24 + 8 * retained_count + 4
~~~

最大文件为 `24 + 8 * 128 + 4 = 1,052` 字节。

### 语义

- `checkpoint_lsn` 是 RDB 快照已经包含的最后一条 mutation/transaction LSN。
- `retained_segment_starts` 为空时，允许 WAL 目录为空；这是初始 bootstrap 的
  manifest 形态。
- 非空时，第一个 retained start 必须等于 `checkpoint_lsn + 1`。
- manifest 引用的每个 segment 必须在恢复时存在。
- manifest 保存从 `checkpoint_lsn + 1` 开始的完整 live segment 集合，而不是
  只保存一个 active segment；后台 checkpoint 在 worker 写 RDB 期间允许继续
  admission，因此最终发布边界可能跨过多个 segment。
- 恢复会扫描第一个 retained start 之后的**所有** segment，包括 manifest 发布后
  才轮转出来的更新 segment。

manifest 没有 snapshot 文件名或 generation 字段：当前 `-r <rdb_path>` 只有一个
快照路径，snapshot 发布早于 manifest 发布，旧 manifest + 新 snapshot 通过 LSN
重放保持一致。

## CRC32C

WAL 和 manifest 共用同一 CRC32C 实现。外层 CRC 覆盖记录除末尾 4 字节校验和之外
的全部字节；MTXN payload 没有独立 CRC，由外层记录 CRC 完整保护。校验和不匹配
视为损坏，不做自动修复。

## 兼容性

- 编码器始终产生 WAL v2；解码器接受 v1 和 v2。
- v1 只允许 standalone `SET/DEL/EXPIRE`；v1 `TXN` 拒绝。
- v2 允许 standalone `SET/DEL/EXPIRE` 和 `TXN`。
- v1 与 v2 记录可以混读，但每个记录的版本独立解释。
- manifest v1 仍是当前唯一接受的 manifest 版本。
- 未知版本、未知 flag 和未知 op 均拒绝。
- 旧 RDB v2 在没有 WAL sidecar 时可以作为 legacy 数据库导入；一旦存在 WAL
  segment 而没有 manifest，拒绝自动迁移。
- 不提供在线格式降级或跨版本滚动升级；回滚到只认识 v1 的旧二进制前，必须
  停止写入并恢复兼容的持久化备份。

## 非法输入矩阵

| 输入 | 结果 |
|---|---|
| magic 错误 | 损坏，拒绝 |
| version 不是 1/2 | 损坏，拒绝 |
| flags != 0 | 损坏，拒绝 |
| LSN = 0 | 损坏，拒绝 |
| LSN 不连续/重复 | 损坏，拒绝 |
| segment 第一条记录 LSN != 文件名 | 损坏，拒绝 |
| key 为空或 > 1024 | 损坏，拒绝 |
| value > 1 MiB | 损坏，拒绝 |
| DEL 带 value/expiry | 损坏，拒绝 |
| EXPIRE 的 expiry <= 0 | 损坏，拒绝 |
| v1 op 为 TXN | 损坏，拒绝 |
| TXN 外层 key 不是 `txn` 或 expiry 非 0 | 损坏，拒绝 |
| MTXN magic/version 错误 | 损坏，拒绝 |
| MTXN op_count 为 0 或 > 256 | 损坏，拒绝 |
| MTXN payload > 1 MiB | 损坏，拒绝 |
| MTXN 重复 key、嵌套 TXN 或未知 op | 损坏，拒绝 |
| MTXN reserved 非 0 或尾部冗余 | 损坏，拒绝 |
| CRC 不匹配 | 损坏，拒绝 |
| 最后一个 segment 的尾部半写 | 截断到最后合法前缀并 fsync |
| 非最后 segment 的半写 | 损坏，拒绝 |
| 完整记录存在但 MTXN payload 非法 | 损坏，拒绝，不发布任何事务操作 |
| 中间 segment 损坏 | 损坏，拒绝 |
| manifest 引用缺失 segment | 损坏，拒绝 |
| manifest 保留列表不以 checkpoint+1 开始 | 损坏，拒绝 |
| 恢复超过记录/字节上限 | 资源上限，拒绝 |

## 相关文档

- [恢复模型](recovery.md)
- [崩溃一致性](crash-consistency.md)
- [ADR-002](decisions/002-wal-and-crash-consistency.md)
- [ADR-004](decisions/004-wal-resource-policy.md)
- [ADR-008](decisions/008-mvcc-transactions.md)
