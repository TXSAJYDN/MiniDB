# ADR-003：持久化模式与返回时机

- 状态：Accepted
- 日期：2026-11-11
- 关联：`docs/architecture.md`、`docs/recovery.md`

## 背景

“写成功”必须对应可解释的持久化语义。如果所有模式都只承诺“可能落盘”，就无法
回答客户端真正关心的问题：收到成功响应后崩溃，数据是否一定还在。

MiniDB 选择少量、可验证的模式，而不是提供一组名称好听但无法证明的选项。

## 决策

实现两种模式，默认 `SYNC`：

| 模式 | CLI | 成功响应条件 | 已确认写保证 | 可能丢失 |
|---|---|---|---|---|
| `SYNC` | `-d sync` | WAL append + 文件 fsync 成功，再应用内存并响应 | 在 fsync/设备模型成立时，跨进程崩溃可恢复 | 对已确认写不主动允许丢失 |
| `INTERVAL` | `-d interval` | WAL append 成功、应用内存后响应 | 依赖周期 flush；checkpoint/正常关闭前强制 flush | 最近一个 flush 窗口内的确认写 |

`INTERVAL` 的 timer 周期是 100 ms，由 owner EventLoop 的 timerfd 驱动。实际丢失
窗口受调度、I/O 和事件循环停顿影响，不能理解为一个精确的 100 ms 承诺。

`BATCHED` 未实现。[ADR-005](005-group-commit.md) 已把 R4-A 定义为
evidence-gated 的组提交设计、诊断指标和准入条件；当前已完成 ADR、baseline
instrumentation 和外部非生产 G3 batch formation 实验，G1-G3 PASS。实现级 ADR
仍待编写，writer thread、有界队列、批量 fsync 和 completion 顺序均未实现；
只有实现级 ADR 和 G4-G7 后续验证完成，才能讨论该模式。

## fsync 失败语义

- WAL append 或 fsync 失败后，持久化层进入 fail-stop，不再接受新 mutation。
- 已推进的 LSN、WAL 字节计数和统计值不回滚。
- 失败的那次 mutation 不返回成功。
- 如果完整 WAL 记录已经写入但 fsync 失败，记录可能已经落盘；恢复允许出现
  未确认写。
- fsync 失败后必须重启，由恢复流程重新判断最后合法前缀。

## 未确认写

测试模型区分：

- ack set：客户端已经收到成功响应的 operation；
- possible set：崩溃前可能已经写入磁盘的 operation。

允许 `possible set` 包含未确认写；不允许 `ack set` 中的 operation 在恢复后缺失。

因此，比较“崩溃前后所有数据完全一致”不是正确验收标准。MiniDB 的崩溃测试验证
的是 ack set 的持久性和未确认写的允许范围。

## 被否决的方案

- 默认 `INTERVAL`：性能更好，但不能作为“确认写不丢”的默认承诺。
- 只提供 `SYNC`：正确性清晰，但无法展示明确的性能/可靠性权衡。
- 立即实现 `BATCHED`：没有基线证据，复杂度和验证成本过高。
- 用 `fdatasync` 替代 `fsync`：当前实现选择更保守的文件 fsync；改变语义需要
  单独实验和文档更新。

## 后果

- `SYNC` 的写吞吐受每次 fsync 限制；本地 WSL2 基准中明显低于 `INTERVAL`。
- `INTERVAL` 明确允许最近写丢失，不能用于“ack 后绝不丢”的场景。
- 两种模式共享同一 WAL、checkpoint 和恢复路径，减少分支差异。