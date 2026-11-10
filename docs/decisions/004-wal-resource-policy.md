# ADR-004：WAL 资源上限与轮转策略

- 状态：Accepted
- 日期：2026-11-11
- 关联：`docs/wal-format.md`、`docs/deployment.md`

## 背景

WAL 不能无限增长。面向有界资源的服务必须把磁盘、恢复内存和 segment 数量变成
硬上限，并在达到上限时有确定行为，而不是等到磁盘耗尽后产生不可解释的失败。

## 决策

默认上限如下，测试通过注入更小的 `WalLimits` 覆盖边界：

| 项目 | 默认值 |
|---|---:|
| 单个 WAL segment | 16 MiB |
| WAL 总字节 | 256 MiB |
| 活跃 segment 数 | 128 |
| 单条 WAL 记录 | 34 + 1 KiB + 1 MiB + 4 B |
| 单个 key | 1 KiB |
| 单个 value | 1 MiB |
| 恢复扫描记录数 | 1,000,000 |
| 恢复扫描字节数 | 512 MiB |
| RDB 快照 | 64 MiB / 100,000 records |

这些值是代码中的默认值，不是对所有部署环境的容量建议。部署方应在测试中验证
自己的磁盘、文件系统和恢复时间预算。

## 轮转

- segment 文件名：`wal-%020llu.log`，数字为该 segment 第一条记录的起始 LSN。
- 一条记录必须能完整放入一个 segment；记录本身大于 segment 上限时拒绝写。
- 当前 segment 放不下一条记录时，先 flush，再关闭旧文件，创建以 `next_lsn`
  为起始 LSN 的空 segment，并 fsync WAL 目录。
- 只有最后一个 segment 可以为空。
- 恢复要求 segment 起始 LSN 连续，且每个 segment 的第一条记录 LSN 等于其文件名。
- manifest 引用第一个 retained segment；恢复会包含该位置之后的所有 segment，
  因此“已轮转但 manifest 尚未更新”的 segment 不会被遗漏。

## 达到上限

写路径在 append 前检查 segment 数。保留一个 segment 余量，以便 checkpoint 总能
先轮转当前 active segment 再发布 manifest。

当 append 返回资源上限时：

1. 尝试执行一次同步 checkpoint；
2. checkpoint 成功后，用同一记录重试一次；
3. checkpoint 失败则拒绝本次写，不返回成功；
4. 持久化层是否 fail-stop 取决于失败阶段：
   - 资源上限本身不标记 fail-stop；
   - I/O 错误和 fsync 失败标记 fail-stop。

不能通过无限增加 segment、绕过上限或静默丢弃记录来“恢复”写入。

## 恢复上限

恢复在读取前检查：

- segment 数量和单 segment 大小；
- 总恢复字节数；
- 解码记录数；
- 单条 key/value 长度；
- LSN 连续性和溢出。

超过上限返回 `resource-limit`，进程非零退出，不监听端口。

## 清理

checkpoint 成功后：

1. 新 snapshot 已 fsync、rename 并 fsync 父目录；
2. 新 manifest 已 fsync、rename 并 fsync 父目录；
3. 才允许删除 manifest 不再引用的旧 segment；
4. 删除后 fsync WAL 目录。

清理失败不使 checkpoint 失效：旧 segment 成为 residue，恢复时被忽略；下一次
checkpoint 会重试清理。

## 被否决的方案

- 达到上限后继续写并依赖磁盘剩余空间：会产生不可预测的 ENOSPC 和恢复失败。
- 自动删除最旧 segment：可能删除 manifest 仍引用的数据。
- 允许超大单记录：会破坏单 segment 上限和恢复内存预算。
- 不限制恢复记录数：损坏文件可能造成长时间启动或 OOM。

## 后果

- WAL 增长有硬上限，但持续写入必须周期性 checkpoint。
- checkpoint 停顿和写放大是可测量的代价。
- 恢复时间是 WAL 字节数和记录数的函数，必须通过基准报告解释。