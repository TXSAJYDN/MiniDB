# ADR-002：WAL、checkpoint 与崩溃一致性

- 状态：Accepted
- 日期：2026-11-11
- 决策者：MiniDB 项目
- 关联：`docs/wal-format.md`、`docs/recovery.md`、`docs/crash-consistency.md`

## 背景

MiniDB 在 R0 之前只有 RDB v2 全量快照。一次成功写入只修改内存，只有显式 SAVE
或正常关闭才会落盘。因此客户端收到写成功响应后，进程崩溃仍可能丢掉该写。

R0-R3 的目标不是把 MiniDB 包装成生产数据库，而是建立一个可以被测试和证伪的
持久化闭环：

> 客户端收到成功响应后，进程在任意点崩溃，已确认写是否还能恢复？

## 决策

采用逻辑 WAL + RDB checkpoint + manifest 的组合：

1. 每个 mutation 在应用内存前先追加一条逻辑 WAL 记录。
2. `SYNC` 模式在 WAL 文件 fsync 成功后才应用内存并返回成功。
3. `INTERVAL` 模式在 WAL append 后即可应用和返回；由 owner loop 的 100 ms
   timer、checkpoint 和正常关闭负责 flush。
4. RDB 只作为 checkpoint，不再是每一笔写入的唯一持久化机制。
5. manifest 原子记录 `checkpoint_lsn` 和必须保留的 WAL segment 起始 LSN。
6. checkpoint 的发布顺序固定为：snapshot 发布 -> manifest 发布 -> 清理旧 WAL。
7. 恢复时先构建临时 KvStore；RDB、manifest 和 WAL 全部验证通过后才发布 Store。

## 记录与 LSN

- WAL 使用逻辑记录：`SET=1`、`DEL=2`、`EXPIRE=3`。
- 每个 mutation 分配严格递增的 `u64` LSN，从 1 开始。
- 单 owner loop 保证 LSN 顺序与命令执行顺序一致。
- `next_lsn = max(最后合法 LSN, checkpoint_lsn) + 1`。
- LSN 达到 `UINT64_MAX` 时 fail-stop，不允许回绕。
- 记录格式、CRC32C 和边界见 `docs/wal-format.md`。

## 单写者与所有权

- 启动时对 `<rdb_path>.lock` 执行 `flock(LOCK_EX | LOCK_NB)`。
- 锁在 PersistenceManager 生命周期内持有，析构时释放。
- 第二个 MiniDB 进程访问同一数据目录时以非零退出码拒绝启动。
- 该锁是 advisory lock：它约束合作进程，不能阻止任意第三方程序直接改文件。

## 恢复规则

1. 获取单写者锁。
2. 若 manifest 缺失：
   - 无 RDB、无 WAL：空库，等待首次 mutation/checkpoint 建立初始镜像；
   - 有 RDB、无 WAL：按 legacy RDB 导入，首次写入时建立 manifest 和 WAL；
   - 有 WAL、无 manifest：拒绝自动猜测，启动失败。
3. 若 manifest 存在但 RDB 缺失：启动失败。
4. 加载 RDB 到临时 Store。
5. 校验 manifest 引用的每个 segment 都存在，且第一个 retained segment 从
   `checkpoint_lsn + 1` 开始。
6. 扫描该位置及之后的全部 segment，包括 manifest 尚未列出的更新 segment。
7. 只重放 `LSN > checkpoint_lsn` 的记录，要求 LSN 连续。
8. 仅允许最后一个 segment 的最后一条记录不完整：截断到最后合法前缀，fsync
   文件和目录。
9. 中间损坏、重复 LSN、LSN 断层、segment 起始不匹配、缺失 segment、CRC 错误
   或资源超限均拒绝启动。
10. 全部成功后交换 Store，并打开 WAL 管理器；在恢复成功前不监听端口。

## 为什么需要 manifest

不能靠文件名或“最新临时文件”猜测快照与日志的对应关系。manifest 是唯一将
RDB checkpoint 与 WAL 区间绑定起来的原子元数据：

- snapshot 必须先于 manifest 可见；
- manifest 只能引用已经持久化的 snapshot 和 segment；
- WAL 清理只能在 manifest 持久化之后发生；
- 崩溃后允许留下“新 snapshot + 旧 manifest”，通过旧 checkpoint LSN 重放 WAL
  得到正确状态；
- 不允许留下“新 manifest + 缺失/未持久化 snapshot”。

## 被否决的方案

- 只保留 RDB：无法保证每次成功写入跨进程崩溃可恢复，SAVE 还会造成长时间停顿。
- 页级物理 WAL：对当前 `unordered_map` 存储过于复杂，且没有页缓存层可复用。
- 无 manifest、按文件名猜最新快照：无法证明 snapshot 与 WAL 的边界。
- 自动跳过损坏记录：会把静默数据损坏伪装成成功恢复。
- 默认组提交：需要额外线程、队列和 TSan/崩溃矩阵验证，属于未启动的 R4。

## 后果

正面：

- 已确认写在 `SYNC` 下可跨进程崩溃恢复；
- `INTERVAL` 有明确、可测量的丢失窗口；
- checkpoint 限制 WAL 增长和恢复时间；
- 恢复失败时不会发布部分 Store。

代价：

- 每次 `SYNC` 写入承担一次 WAL fsync，吞吐显著低于纯内存路径；
- checkpoint 仍是 stop-the-world，会暂停 owner loop；
- 需要维护 manifest、segment 清理和更多故障测试；
- 只证明进程崩溃语义，不证明掉电、内核崩溃或设备缓存撒谎。