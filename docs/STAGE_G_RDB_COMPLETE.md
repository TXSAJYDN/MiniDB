# MiniDB 阶段 G：RDB 快照与启动恢复验收完成

- 验收日期：2026-11-06
- 代码基线：`9da4b48f5fe78e4c2896ad1ca07464361b27e362`
- 阶段状态：**PASS**
License: DEFERRED BY OWNER

## 1. 实现范围

RDB v2 快照包含：

- `MRDB` magic 和网络字节序版本/记录数；
- LEB128 varint 长度编码；
- CRC32C（Castagnoli）校验；
- 二进制安全的 key/value；
- expiry 字段和 TTL 恢复；
- 精确消费和尾随字节拒绝；
- 临时文件写入、文件 fsync、原子 rename、父目录 fsync；
- 失败时保留旧快照；
- rename 后目录持久化失败时返回 `DURABILITY_UNCONFIRMED`。

## 2. 故障注入与恢复矩阵

`RdbManagerFaultTest` 已在六配置 CTest 中通过，覆盖：

- open 失败；
- 部分写后 ENOSPC；
- 文件 fsync 失败；
- close 失败；
- rename 失败；
- EINTR 和短写重试；
- EEXIST 重试与重试耗尽；
- rename 后父目录 open 失败；
- rename 后目录 fsync 失败；
- 并发 SAVE 最大并发度为 1；
- 写作阶段 kill -9 后恢复完整旧快照；
- rename 后 kill -9 恢复完整新快照；
- SAVE 前 marker 在 reload 后仍存在；
- SAVE 暂停 scanner，完成后恢复；
- 失败 SAVE 释放 scanner pause。

格式/加载测试还覆盖：

- Varint 单字节、多字节、最大值、第五字节 continuation 和 payload 溢出；
- CRC32C 空输入、已知向量和确定性；
- 网络字节序 32/64 位；
- 截断记录、bit flip、尾随字节；
- 重复 key、record count 不匹配、版本错误；
- 非空 Store 加载失败后 key/value/TTL 全部不变。

## 3. 64 MiB 验收

`tools/run_stage_g_acceptance.py` 在 Release 构建上生成恰好达到格式上限的 RDB，并测量恢复和在线 SAVE：

| 指标 | 结果 |
|---|---|
| 文件大小 | `67,108,864` 字节（恰好 64 MiB） |
| record count | 64 |
| 在线 SAVE 后、恢复启动前 SHA-256 | `4b399a1decfaa878950fc20ba22d164d1984e170069a783798945d5f06c4f555` |
| 恢复服务优雅退出重写后 SHA-256 | `e8966c16b6f98db63e650a4cb4468f3bb908ff59eef421fc4ede0b38678dab21` |
| 启动恢复 keys | 64/64 |
| 在线 SAVE | HTTP 200 |
| SAVE 请求耗时 | 1265.410 ms |
| 在线 SAVE 峰值 RSS | 200.785 MiB |
| 启动恢复峰值 RSS | 132.621 MiB |
| RSS 预算 | 512 MiB |
| SAVE 暂停预算 | 30 秒 |
| 临时文件残留 | 0 |

第一个摘要是启动恢复实际加载的文件；恢复进程收到 SIGTERM 后按正常关停流程再次执行 SAVE，所以最终文件被重写为第二个摘要。两次文件大小均为精确 64 MiB，第二个摘要不表示恢复失败。

所有断言均通过，`failed_assertions` 为空。

## 4. 证据位置

```text
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-g/stage-g-summary.json
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-g/rss-online-save.jsonl
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-g/rss-startup-recovery.jsonl
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-g/logs/
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-g/work/stage-g-max.rdb
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-g/stage-g-post-recovery.sha256
```

对应 CTest 日志位于六配置证据目录的 `ctest.log`。

## 5. 限制

- 只支持 RDB v2，没有 v1 迁移。
- 没有跨进程文件锁；同一 RDB 路径只允许一个 server 进程。
- 64 MiB 和 512 MiB/30 秒是冻结的验收预算，不是对所有硬件/负载的生产保证。
- kill -9 测试验证进程崩溃恢复，不等同于断电或文件系统级故障注入。

## 6. 判定

**Stage G：PASS。**