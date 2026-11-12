# Changelog

本项目从 v0.1.0 起使用语义化版本；`0.x` 表示接口与持久化格式仍可能演进，不构成生产稳定性承诺。

## [0.1.0] - 2026-11-12

MiniDB 首个公开正式基线：面向有界资源的单机 crash-consistent KV 存储服务。

### 核心能力

- HTTP/1.1 子集与二进制安全 TCP KV 前端，共享同一持久化引擎；
- `SYNC`/`INTERVAL` durability、CRC32C WAL、严格 LSN、segment 轮转、原子 manifest 与 RDB v2 checkpoint；
- owner EventLoop + 专用 writer thread，有界组提交与异步 mutation/checkpoint；
- Snapshot Isolation、first-committer-wins、per-key 版本链与单记录 `kTxn` 原子事务；
- 二进制 `AGENT=12` 持久 Agent 状态后端，包含 session/tool/lease/fence/generic record/idempotency；
- C01-C14 崩溃点、恢复 fail-stop、单写者锁、TTL/GC、资源上限和诊断 stats。

### 质量门禁

- Debug、Release、ASan、UBSan、ASan+UBSan、TSan 六配置各 440/440 CTest；
- 31 个进程级集成用例；
- HTTP、KV、WAL、manifest、Agent record、Agent protocol 六个固定 seed fuzz target；
- GitHub Actions 9/9：debug、release、asan、ubsan、asan-ubsan、tsan、format、static-analysis、fuzz-smoke。

### 文档与发布

- ADR-005 至 ADR-009，以及架构、WAL、RDB、恢复、崩溃一致性、协议、部署和可靠性证据文档；
- 首个发布采用 Apache License 2.0，版权与归属见 `NOTICE`。

### 已知边界

- 这是可靠性工程原型，不是生产 SLA、高并发、复制、分布式共识或安全边界完备的数据库；
- 不提供 SQL、Redis 兼容、TLS/认证/授权、多写者、多节点 lease 或外部工具副作用 exactly-once；
- crash-consistent 结论只覆盖已测试的进程崩溃与重启，不证明掉电、内核崩溃或介质损坏。

[0.1.0]: https://github.com/TXSAJYDN/MiniDB/releases/tag/v0.1.0
