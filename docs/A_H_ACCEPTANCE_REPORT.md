# MiniDB 阶段 A-H 独立验收报告

- 报告日期：2026-11-06（Asia/Shanghai）
- 权威工作树：`/mnt/e/c++/miniDB`（Windows：`E:\c++\miniDB`）
- 远程仓库：`git@github.com:TXSAJYDN/MiniDB.git`
- 分支：`main`
- 验收代码基线：`9da4b48f5fe78e4c2896ad1ca07464361b27e362`
- 环境：Ubuntu 24.04 WSL2 ext4，GCC 13.3.0，CMake 3.28.3，Clang 18.1.3
- 总体结论：**阶段 A-H 技术验收 PASS**
License: DEFERRED BY OWNER

## 1. 结论先行

**MiniDB 阶段 A-H 已整体通过本轮独立技术验收。**

本轮不是以自评报告或单个 happy-path 测试作为通过依据，而是以仓库外全新构建、真实 socket 进程、故障注入、sanitizer、静态检查、fuzz、进程 kill -9 恢复、真实 GitHub Actions 和公开贡献尝试记录为依据。最终结论如下：

| 阶段 | 判定 | 关键依据 |
|---|---|---|
| A 工程基线与工具链 | **PASS** | 源码/测试/CI 已提交并推送；六配置矩阵 214/214；许可证由所有者暂缓 |
| B Socket 与非阻塞 I/O 实验 | **PASS** | Debug/ASan/UBSan 三套真实 echo，EAGAIN、短写、RST、fd 与 shutdown 证据完整 |
| C LT Reactor 与连接生命周期 | **PASS** | EMFILE/ENFILE、epoll_ctl 失败、fd 复用、HUP/RDHUP、回调销毁和 shutdown 专项通过 |
| D Session 边界与背压 | **PASS** | 分片输入、100 次 Keep-Alive、慢读 1 MiB、EPOLLIN gating 和超时关闭通过 |
| E HTTP 子集与 HTTP 服务 | **PASS** | parser 状态/上限矩阵、Host/CL/TE、路由/方法/状态码、持久化和 SAVE 失败进程测试通过 |
| F KV、Store 与 TTL | **PASS** | 表驱动 TTL、墙钟跳变、固定 seed 6000 步 model stress、scanner 交错与租约通过 |
| G RDB 快照与启动恢复 | **PASS** | EINTR/短写/ENOSPC/open/fsync/close/rename/dir-fsync 故障矩阵、kill -9、64 MiB 预算通过 |
| H 集成 QA、性能、文档与贡献 | **PASS** | CI 9/9、fuzz 20k×2、静态检查、格式、benchmark、strace、公开贡献真实尝试均记录 |

两个必须同时说明的边界：

1. 报告头中的许可证字段是项目所有者明确作出的交付决定，不是技术验收失败，也不代表仓库已经获得某个开源许可证。
2. 公开贡献要求的是“真实尝试”。本轮已创建并验证补丁，但上游和 fork 推送均被权限拒绝，匿名 PR 返回 401；因此记录为“尝试完成、PR 未创建”，不能写成贡献已提交或被接受。

## 2. 验收口径与可追溯性

统一证据根目录：

```text
/home/win11/minidb-ah-final-20261106-v3-evidence
```

矩阵源码快照：

| 项目 | 值 |
|---|---|
| 路径 | `/mnt/e/c++/miniDB` |
| 文件数 | 653 |
| SHA-256 | `2dba58c83beca499affefa42bcf6b970d2b0c83f01c5bcd6e24de75fc0b73843` |
| 矩阵时 Git HEAD | `0bf20e1c45eada3b4ee8f80ddb70c6e9454ef2a6` |
| 提交后的验收基线 | `9da4b48f5fe78e4c2896ad1ca07464361b27e362` |

矩阵采集时工作树中的 19 个源码/测试/工具变更随后原样提交为 `9da4b48`，因此矩阵结果可对应到已推送的代码内容。提交统计为 19 files changed、1172 insertions、60 deletions。

> 本报告不把“文件存在”当作通过。凡是计为 PASS 的专项，均要求对应测试进入 CTest 或对应外部证据包含退出码、输入、输出和可复核日志。

## 3. 阶段 A：工程基线与工具链

### 3.1 已核实

- `main` 已推送至 GitHub `TXSAJYDN/MiniDB`。
- 当前 `main` 跟踪 419 个文件，包含 `src/`、`app/`、`tests/`、`fuzz/`、`experiments/`、`CMakeLists.txt`、CMake presets、`.github/workflows/ci.yml` 和交付文档。
- 六种配置在仓库外全新目录完成 configure/build/CTest：
  - Debug、Release、ASan、UBSan、ASan+UBSan、TSan；
  - 每项 configure=0、build=0、ctest=0、214/214。
- ASan+UBSan 编译命令同时包含 address 和 undefined sanitizer，测试链接命令也同时包含两种标志；TSan 编译与链接标志经过核实。
- TSan 在 WSL2 上使用 `setarch x86_64 -R` 规避 ASLR 启动失败，并在该条件下完成 214/214。
- 真实 GitHub Actions run 42182440543 对代码基线执行了 9 个 job，全部成功。

### 3.2 判定

**PASS。**

许可证没有被擅自创建或选择。所有者决定暂缓许可证，交付文档统一记录该决定，这不阻断技术验收。

## 4. 阶段 B：Socket 与非阻塞 I/O 实验

### 4.1 证据

`experiments/socket_echo_blocking.cpp`、`experiments/socket_echo_nonblocking.cpp` 和 `experiments/echo_client.py` 存在于已跟踪仓库中，并由验收脚本启动真实 socket 进程。

三套配置均执行 blocking 与 non-blocking echo：

| 配置 | accepted | EAGAIN | 短写 | RST | fd before/after | 结果 |
|---|---:|---:|---:|---:|---|---|
| Debug | 30 | 1073 | 16 | 1 | 4/4 | PASS |
| ASan | 30 | 905 | 18 | 1 | 4/4 | PASS |
| UBSan | 30 | 1585 | 51 | 1 | 4/4 | PASS |

- 三套配置客户端退出码均为 0；
- ASan/UBSan 未产生诊断；
- fd 数量在运行前后均回到 4，未观察到泄漏；
- strace 捕获到 `accept4`、EAGAIN、`shutdown`、`close` 和 `sendto`；
- Debug 证据目录：`stage-b-debug/summary.json`；
- ASan/UBSan 证据目录：`stage-b-asan/summary.json`、`stage-b-ubsan/summary.json`。

### 4.2 判定

**PASS。**

阶段 B 所要求的阻塞/非阻塞行为、短写、EAGAIN、EINTR、EOF/RST 和 fd 生命周期已经由真实进程证据覆盖。

## 5. 阶段 C：LT Reactor 与连接生命周期

### 5.1 已关闭的风险

六配置 CTest 中包含以下直接对应阶段 C 的测试：

- Acceptor：
  - `EintrIsRetriedAndEagainDoesNotPause`
  - `EmfileClosesIdleAndDummyFdThenPauses`
  - `EnfileClosesIdleAndDummyFdThenPauses`
  - `ResourceExhaustionWithoutReservedIdleFdStillPauses`
  - `RetryTimerResumesAccepting`
  - `PausesAfterExactly1024AcceptsThenResumes`
  - `AcceptedFdIsClosedWhenNoCallbackIsInstalled`
  - `UnexpectedAcceptErrorStopsOnlyCurrentDrain`
- Poller：
  - `DelFailureStillRetiresLocalRegistration`
  - `StaleTokenSurvivesFdReuseWithoutDereferencingOldChannel`
  - `ModEnoentReAddUsesFreshTokenAndDiscardsOldEvent`
  - `AddFailureDoesNotPublishTokenAndCanBeRetried`
  - `BatchSkipsChannelDestroyedByEarlierCallback`
  - `DestructorClosesInjectedEpollDescriptor`
- Channel：
  - `DispatchesErrorReadWriteAndCloseInContractOrder`
  - `HupWithoutInStillDrainsResidualPayloadBeforeClose`
  - `RdhupDrainsBeforeSingleCloseCallback`
  - `ErrorWithoutReadOrCloseBitsDoesNotInvokeThoseCallbacks`
  - `CallbackMayDestroyChannelWithoutLaterCallbackAccess`
  - `MissingCallbacksAreIgnored`
- TcpConnection：
  - `FinDeliversResidualPayloadBeforeClosingOnce`
  - `ShutdownIsIdempotentAndClosesOnce`
  - `SingleSendAboveBudgetIsRejectedWithoutAllocation`
  - `CumulativeBufferedOutputRespectsBudget`
  - `EarlyDestructionReleasesDescriptor`

这些测试在 Debug、Release、ASan、UBSan、ASan+UBSan 和 TSan 六配置中全部通过。TSan 未报告数据竞争，sanitizer 未报告 use-after-free 或越界。

### 5.2 判定

**PASS。**

阶段 C 的 accept 资源边界、epoll_ctl 错误、fd 复用、HUP/RDHUP 残余数据、回调销毁和幂等关闭均已形成可复核回归。

## 6. 阶段 D：Session 边界与背压

### 6.1 已核实

`HttpSessionIntegrationTest` 覆盖：

- `FragmentedInputAndHundredKeepAliveRequests`
- `PipelinedHundredRequestsPreserveOrder`
- `IdleTimeoutReturns408AndClosesConnection`
- `SlowReadDrainsOneMiBResponseCompletely`
- `PeerHupResetAndEpipeDoNotStopServer`
- `CompleteRequestThenShutdownWriteReturnsResponse`
- `OneMiBEchoThenShutdownWriteSlowReadGetsCompleteResponse`

`TcpConnectionTest.StopReadingBlocksDeliveryUntilStartReading` 直接验证 stop/start reading 的交付门控。输出预算、单次超大发送拒绝、累积缓冲预算和关闭幂等也由阶段 C 列出的 TcpConnection 测试关闭。

### 6.2 判定

**PASS。**

阶段 D 的 Keep-Alive、慢读、背压、EPOLLIN gating、异常断开和资源归还已有专项测试，不再只是集成测试的间接观察。

## 7. 阶段 E：HTTP 子集与 HTTP 服务

### 7.1 parser 和 session 覆盖

`HttpParserP0Test` 的 16 项测试覆盖：

- 合法 POST、Host 唯一性、二进制 body；
- 重复/冲突 Content-Length、非法 framing、升级特性交由 session 拒绝；
- version/request-line 状态映射；
- malformed header；
- request target 414、header 431、header 字段数 64；
- body 上限、精确 1 MiB、未完整 body 等待更多数据；
- 65537 字节未结束 request-line/header 分片；
- 多条合法 header 累计超限。

`HttpSessionIntegrationTest` 覆盖 health/路由/方法矩阵、KV CRUD、Echo/Save、100 次 Keep-Alive、100 次 pipeline、Host 错误、framing/upgrade 错误、状态码与大小限制、Connection: close、超时、慢读和 RST/EPIPE。

`ProcessIntegrationTest` 覆盖 CLI 契约、HTTP/KV 重启持久化、TTL scanner 生命周期、损坏 RDB 启动失败和 SAVE 失败退出码。

### 7.2 判定

**PASS。**

HTTP P0 子集、状态契约、大小边界、连接语义和服务级进程行为均已有可执行证据。

## 8. 阶段 F：KV、Store 与 TTL

### 8.1 已核实

表驱动/边界测试：

- `DeadlineBoundaryIsStrictlyNowLessThanDeadline`
- `LiveSubMillisecondDeadlineReportsZeroInsteadOfMissing`
- `SetClearsExistingTtl`
- `ExpireMissingAndExpiredKeysReturnFalse`
- `NonPositiveExpireRemovesLiveKeyAndIsIdempotentWithTtl`
- `TtlCommandsReturnProtocolValuesAndRoundDown`
- `ExportOmitsOnlyActuallyExpiredEntries`
- `WallClockJumpsDoNotChangeMonotonicExpiry`
- `RestoreUsesAbsoluteWallDeadlineAndRejectsEmptyKey`

并发/交错/压力测试：

- `FixedSeedModelMatchesCommandAndSweepSemantics`：固定 seed，6000 步命令与周期性 sweep 的模型对照；
- `ScannerAndSameKeyMutationsRemainLiveAndConverge`；
- `ScannerLeasesShareOneThreadUntilLastRelease`；
- `ManualOwnerAndLeaseDoNotStopEachOther`；
- `ScannerCanBeStoppedAndRestartedWithoutLeakingAThread`；
- `ServerScannerLeasesAreReferenceCounted`；
- `ServerDestructorsReleaseScannerLeases`。

KV 线协议还覆盖分片帧、pipeline 精确消费、最大 key/value、非法长度、尾随字节、未知命令、二进制值和 TTL 参数验证。

### 8.2 判定

**PASS。**

TTL 语义、scanner 交错、租约与固定 seed model stress 均已进入 214 项 CTest，并在 TSan 下通过。

## 9. 阶段 G：RDB 快照与启动恢复

### 9.1 故障注入矩阵

`RdbManagerFaultTest` 覆盖：

- `OpenFailurePreservesOldSnapshot`
- `PartialWriteThenEnospcPreservesOldSnapshot`
- `FileFsyncFailurePreservesOldSnapshot`
- `CloseFailurePreservesOldSnapshot`
- `RenameFailurePreservesOldSnapshot`
- `EintrAndShortWritesAreRetriedUntilComplete`
- `EexistIsRetriedWithANewTemporaryPath`
- `EexistExhaustionFailsAfterBoundedRetries`
- `ParentOpenFailureAfterRenameIsDurabilityUnconfirmed`
- `DirectoryFsyncFailureAfterRenameIsDurabilityUnconfirmed`
- `ConcurrentSavesSerializeAtMaximumConcurrencyOne`
- `KillNineAtWriteAndFsyncPhasesRecoversCompleteOldSnapshot`
- `KillNineAfterRenameRecoversCompleteNewSnapshot`
- `PreSaveMarkerSurvivesReload`
- `SavePausesScannerUntilFileIoCompletesThenResumesSweeps`
- `FailedSaveReleasesScannerPause`

RDB 格式和加载测试还覆盖 Varint 边界/真实溢出、CRC32C、网络字节序、截断、bit flip、尾随字节、重复 key、record count、版本错误和非空 Store 失败不变性。

### 9.2 64 MiB 预算与恢复

`tools/run_stage_g_acceptance.py` 使用 Release 构建生成恰好 64 MiB 的 RDB v2：

- 文件大小：`67,108,864` 字节，恰好 64 MiB；
- record count：64；
- 在线 SAVE 后、恢复启动前 SHA-256：`4b399a1decfaa878950fc20ba22d164d1984e170069a783798945d5f06c4f555`；
- 启动恢复 64 keys 完整；
- 在线 SAVE HTTP 200，耗时 1265.410 ms；
- 在线 SAVE 峰值 RSS 200.785 MiB；
- 启动恢复峰值 RSS 132.621 MiB；
- 进程 RSS 预算 512 MiB，SAVE 暂停预算 30 秒；
- 恢复服务优雅退出时再次正常 SAVE，随后独立审计 SHA-256：`e8966c16b6f98db63e650a4cb4468f3bb908ff59eef421fc4ede0b38678dab21`，文件大小仍为 `67,108,864` 字节；
- 首次在线 SAVE 后无 `.tmp.*` 临时文件残留。

两个摘要对应不同时间点：`4b399...` 是启动恢复实际加载的文件；恢复进程收到 SIGTERM 后按正常关停流程再次 SAVE，因此最终路径被重写为 `e896...`。这不是恢复失败，也不能用后者反推前者未经验证。

证据：`stage-g/stage-g-summary.json`、RSS JSONL、server logs、RDB 文件、首次 SAVE 的进程内摘要和 `stage-g/stage-g-post-recovery.sha256`。

### 9.3 判定

**PASS。**

阶段 G 的文件操作故障、旧快照保留、durability-unconfirmed 语义、并发保存、kill -9 阶段恢复、TTL/值恢复和 64 MiB 预算均已形成可复核证据。

## 10. 阶段 H：集成 QA、性能、文档与公开贡献

### 10.1 真实 CI

- Run ID：`42182440543`
- URL：<https://github.com/TXSAJYDN/MiniDB/actions/runs/42182440543>
- 代码提交：`9da4b48f5fe78e4c2896ad1ca07464361b27e362`
- 结论：Success
- 用时：2m 54s
- 结果：9/9 job 成功

9 个 job 由以下组成：

1. build-and-test matrix 的 Debug；
2. Release；
3. ASan；
4. UBSan；
5. ASan+UBSan；
6. TSan；
7. format；
8. static-analysis；
9. fuzz-smoke。

运行页面有 9 条 `actions/checkout@v4` 的 Node.js 20 deprecation warning。它们是非失败警告，不改变 Success 结论，也不能被描述为“CI 零警告”。

### 10.2 本地 QA

- fuzz：HTTP 20,000 runs、KV 20,000 runs，固定 seed 20261105，退出码 0，artifact 0；
- replay：HTTP 427 个输入、KV 42 个输入，退出码 0；
- clang-format：81 文件，退出码 0；
- clang-tidy：25 文件，0 失败；
- benchmark：HTTP 2344.653 req/s、P99 654.924 us；KV 2400.599 req/s、P99 771.885 us；
- strace：观测 `accept4`、`epoll_wait`、`epoll_ctl`、`read`、`write`、`openat`、`fsync`、`rename`、`close` 和 EAGAIN；
- perf 硬件计数器：当前 WSL2 不支持，明确记录 `No supported events found`；软件计数器成功，不声称 cycles/instructions/branches/cache-misses。

### 10.3 公开贡献真实尝试

目标 issue：

- <https://github.com/eclipse-4diac/4diac-forte/issues/1135>
- `no func_MUX.h, although the 4diac IDE 3.3.0 exports an include for it`

补丁工作树：`/home/win11/minidb-public-contribution-4diac`
基线：`efcd63c957ec191cab4fd57f8c5d6e3a3a30e0e2`
提交：`ac80baaf7df26ff1f3ae50d28b19acece4db9ccf`
变更：4 files changed，48 insertions(+)

补丁新增缺失的 `func_MUX.h`，更新聚合头和 CMake 列表，并加入 INT/UINT/BYTE/BOOL/TIME 测试。定向测试、完整 CTest（1/1，123.54 秒）、clang-format 和 `git diff --check` 均通过。

推送/PR 尝试的真实结果：

- 上游推送：退出 128，`Permission to eclipse-4diac/4diac-forte.git denied to TXSAJYDN`；
- fork 推送：退出 128，`Repository not found`；
- `TXSAJYDN/4diac-forte` 公开 API：404；
- 匿名 PR 创建：HTTP 401 `Requires authentication`。

因此公开贡献门槛按“真实尝试并保留失败证据”判定通过；**没有创建 PR，也没有声称上游接受**。Eclipse ECA 状态未被伪造。

### 10.4 判定

**PASS。**

CI、fuzz、静态检查、格式、性能记录、syscall 证据、文档和公开贡献尝试均已关闭。硬件计数器限制与 PR 认证失败已如实记录。

## 11. 未关闭但不阻断本轮技术验收的限制

这些限制不得被隐藏，也不得被扩大为“已解决”：

1. **许可证暂缓**：没有 `LICENSE`/`COPYING`；报告头的许可证字段只是所有者决定，不是许可证文本。
2. **公开贡献未成功推送**：补丁存在且测试过，但没有 PR；原因是权限、fork 不存在和匿名 API 认证失败。
3. **perf 硬件计数器不可用**：只记录软件计数器，没有 cycles/instructions/branches/cache-misses 或 flame graph 数据。
4. **性能数据环境有限**：单连接 loopback WSL2，不代表饱和吞吐、多客户端扩展性或生产 SLA。
5. **项目非生产就绪**：无 TLS、认证、授权、复制、集群、分片、事务、多进程 RDB 文件锁等。

## 12. 最终判定

**MiniDB 阶段 A-H：PASS。**

判定范围为原计划的 P0 技术验收：工程基线、非阻塞 I/O、Reactor/连接生命周期、Session/背压、HTTP 子集、KV/TTL、RDB 恢复和集成 QA/交付。许可证暂缓、公开贡献未成功推送和 perf 硬件限制均已记录为真实外部限制，不影响本轮技术通过结论。

A-H 通过不等同于生产就绪。后续若进入生产化阶段，应另行规划许可证、认证/TLS、复制/一致性、多进程所有权、压力测试和跨平台验证。