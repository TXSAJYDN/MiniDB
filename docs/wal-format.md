# WAL 与 checkpoint manifest 格式

本文描述 MiniDB 当前的 WAL v1 记录和 checkpoint manifest v1 的字节级布局。所有
多字节整数均为**大端序（big-endian）**，CRC 使用 CRC32C（Castagnoli 反射多项式）。

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

## WAL 记录 v1

### 布局

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `MWAL` |
| 4 | 2 | version | `u16`，必须为 1 |
| 6 | 2 | flags | `u16`，v1 必须为 0；未知 flag 拒绝 |
| 8 | 8 | lsn | `u64`，严格大于 0 |
| 16 | 1 | op | `SET=1`、`DEL=2`、`EXPIRE=3` |
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

最大记录为 `34 + 1024 + 1,048,576 + 4 = 1,049,638` 字节。

### 操作约束

| op | value_len | expiry_ms | 语义 |
|---|---:|---:|---|
| `SET=1` | 0..1 MiB | 0 或正绝对截止时间 | 设置 key；当前 mutation 路径写入 0，表示清除 TTL |
| `DEL=2` | 0 | 0 | 删除 key |
| `EXPIRE=3` | 0 | 必须 > 0 | 设置绝对 TTL deadline |

未知 op、未知 version、未知 flag、非零 reserved、负 expiry、空 key、越界长度、
CRC 错误均拒绝。

`SET` 在格式上允许正 expiry，以便恢复层使用 `set_with_expiry`；当前命令路径的
SET 会清除 TTL，因此实际写入的 expiry 为 0。

### 编码与解码状态

- `kOk`：完整记录，返回消费字节数；
- `kNeedMore`：输入是截断前缀，只有恢复层的最后一个 segment 可以把它当作尾部；
- `kCorrupt`：头部或完整记录非法，恢复拒绝启动。

解码器不会跳过损坏记录，也不会在 CRC 失败后重新同步。

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

- `checkpoint_lsn` 是 RDB 快照已经包含的最后一条 mutation LSN。
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

WAL 和 manifest 共用同一 CRC32C 实现。CRC 覆盖记录/文件除末尾 4 字节校验和之外
的全部字节。校验和不匹配视为损坏，不做自动修复。

## 兼容性

- WAL v1 和 manifest v1 是当前唯一接受的版本。
- 未知版本、未知 flag 和未知 op 均拒绝。
- 旧 RDB v2 在没有 WAL sidecar 时可以作为 legacy 数据库导入；一旦存在 WAL
  segment 而没有 manifest，拒绝自动迁移。
- 不提供在线格式降级或跨版本滚动升级。

## 非法输入矩阵

| 输入 | 结果 |
|---|---|
| magic 错误 | 损坏，拒绝 |
| version != 1 | 损坏，拒绝 |
| flags != 0 | 损坏，拒绝 |
| LSN = 0 | 损坏，拒绝 |
| LSN 不连续/重复 | 损坏，拒绝 |
| segment 第一条记录 LSN != 文件名 | 损坏，拒绝 |
| key 为空或 > 1024 | 损坏，拒绝 |
| value > 1 MiB | 损坏，拒绝 |
| DEL 带 value/expiry | 损坏，拒绝 |
| EXPIRE 的 expiry <= 0 | 损坏，拒绝 |
| CRC 不匹配 | 损坏，拒绝 |
| 最后一个 segment 的尾部半写 | 截断到最后合法前缀并 fsync |
| 非最后 segment 的半写 | 损坏，拒绝 |
| 中间 segment 损坏 | 损坏，拒绝 |
| manifest 引用缺失 segment | 损坏，拒绝 |
| manifest 保留列表不以 checkpoint+1 开始 | 损坏，拒绝 |
| 恢复超过记录/字节上限 | 资源上限，拒绝 |

## 相关文档

- [恢复模型](recovery.md)
- [崩溃一致性](crash-consistency.md)
- [ADR-002](decisions/002-wal-and-crash-consistency.md)
- [ADR-004](decisions/004-wal-resource-policy.md)