# MiniDB 阶段 A-H 独立验收报告

- 报告日期：2026-11-06（Asia/Shanghai）
- 权威工作树：`/home/win11/projects/miniDB`
- 分支/HEAD：`main` / `e4c8d60`
- 环境：Ubuntu 24.04 WSL2 ext4，GCC 13.3.0，CMake 3.28.3，Clang 18.1.3
- 总体结论：**FAIL**

## 1. 结论先行

**MiniDB 阶段 A-H 尚未整体通过。**

当前本地实现和自动化测试已经具备较强覆盖：六种配置各 152/152、AI-018 四项专项回归各 4/4、HTTP/KV fuzz 各 20,000 runs、clang-tidy 25 个文件无失败、benchmark 和 strace 证据可复核。这些证据证明代码与测试并非空壳，但不能替代项目计划中的阶段门槛。

本轮独立验收同时确认了以下阻塞：

- 阶段 A：没有 `LICENSE`，没有可审阅的实现提交；Git 只跟踪了 2 个计划文档。
- 阶段 B：`experiments/` 源码存在，但阶段完成声明引用的 README 和原始 syscall/fd 故障记录缺失。
- 阶段 C：EMFILE/ENFILE、`epoll_ctl` 失败、HUP 残余数据、RDHUP drain 等专项矩阵未完整保留。
- 阶段 D/E：本地集成测试较强，但缺少逐项正式关闭证据，不能标为 PASS。
- 阶段 F：缺少表驱动 TTL fake-clock 边界、scanner 交错和固定 seed model-based stress + TSan 证据。
- 阶段 G：缺少 EINTR/短写/ENOSPC/open/close/fsync/rename/dir-fsync 故障注入及 kill -9 恢复矩阵。
- 阶段 H：真实 CI 运行数为 0，公开 issue/PR 记录为 0；远程仓库根目录只有 `docs`，没有源码或许可证。

因此必须区分两个结论：

- **AI-018：PASS**，见 `docs/AI_018_EXECUTION_REPORT.md`。
- **阶段 A-H：FAIL**。

## 2. 阶段判定总表

| 阶段 | 判定 | 主要依据 |
|---|---|---|
| A 工程基线 | **FAIL** | 无 LICENSE；无可审阅实现提交；实现长期未跟踪。 |
| B Socket/非阻塞 I/O 实验 | **FAIL（证据不足）** | 实验源码和客户端存在，但声明的 README、strace 片段、fd/ASan/UBSan 故障记录未保留。 |
| C LT Reactor 与连接生命周期 | **FAIL（未完整验收）** | 基础网络层和集成测试存在，但错误注入及 HUP/RDHUP 专项矩阵不完整。 |
| D Session 边界与背压 | **未正式关闭** | Keep-Alive、慢读、超时和异常连接测试较强；EPOLLIN gating、stop 幂等和资源归还尚无完整专项闭环。 |
| E HTTP 子集与服务 | **未正式关闭** | 解析、路由、状态码和 socket 集成覆盖较强；逐字段一字节碎片及完整状态契约矩阵未形成独立验收证据。 |
| F KV、Store 与 TTL | **FAIL（未完整验收）** | 协议和集成测试存在，但缺 TTL 表驱动边界、scanner 交错与固定 seed stress/TSan。 |
| G RDB 与恢复 | **FAIL（未完整验收）** | 格式、CRC、损坏样本和原子发布测试较强；文件 I/O 故障注入、kill -9 和资源预算矩阵缺失。 |
| H 集成 QA、性能、文档与开源贡献 | **FAIL** | 本地 fuzz/benchmark/文档有进展，但真实 CI 为 0、无公开 issue/PR、无 LICENSE，交付未进入远程仓库。 |

## 3. 已核实的正向证据

### 3.1 六配置测试矩阵

`/home/win11/minidb-final-evidence-v2/matrix-summary.json` 记录：

| 配置 | configure | build | CTest | 测试数 |
|---|---:|---:|---:|---:|
| Debug | 0 | 0 | 0 | 152/152 |
| Release | 0 | 0 | 0 | 152/152 |
| ASan | 0 | 0 | 0 | 152/152 |
| UBSan | 0 | 0 | 0 | 152/152 |
| ASan+UBSan | 0 | 0 | 0 | 152/152 |
| TSan | 0 | 0 | 0 | 152/152 |

TSan 使用 `setarch x86_64 -R` 运行。ASan+UBSan 与 TSan 的编译命令和测试链接命令均包含对应 sanitizer 标志。

### 3.2 AI-018 专项回归

`/home/win11/minidb-final-evidence-v2/targeted/summary.json` 记录四项测试在六种配置中均为 4/4：

- `HttpParserP0Test.AccumulatedCompleteHeadersOverLimitMapsTo431`（#48）
- `HttpParserP0Test.UnterminatedRequestLineBeyond64KiBIsRejectedAs414`（#52）
- `HttpParserP0Test.UnterminatedHeaderBeyond64KiBIsRejectedAs431`（#53）
- `RdbManagerTest.Load_RealVarintOverflowPreservesNonEmptyStore`（#82）

其中 RDB 样本使用 v2 格式、真实第五字节 varint 溢出、35 字节 header+record、39 字节完整文件和 CRC32C `0x000f14c1`，并验证非空 Store 在失败后保持不变。

### 3.3 其他本地证据

- Fuzz：HTTP 20,000 runs、KV 20,000 runs，seed `20261105`，退出码 0，artifact 为空。
  - `/home/win11/minidb-fuzz-evidence-v2/summary.json`
- clang-tidy：25 个文件，0 失败，退出码 0。
  - `/home/win11/minidb-static-evidence-v2/clang-tidy-v2.log`
- clang-format：`FORMAT_EXIT=0`。
  - `/home/win11/minidb-final-evidence-v2/clang-format-20261106.log`
- Release benchmark：HTTP 2611.453 req/s，KV 2662.157 req/s；每协议 200 warmup、2000 样本。
  - `/home/win11/minidb-final-evidence-v2/benchmark/release-wsl2-20261105.json`
- strace 观测到 `accept4`、`epoll_wait`、`epoll_ctl`、`read`、`write`、`openat`、`fsync`、`rename`、`close`。
  - `/home/win11/minidb-final-evidence-v2/strace/strace-summary.txt`
- perf 硬件计数器不可用；未编造 cycles/instructions 数据。软件计数器成功记录 task-clock、page-faults、context-switches 和 cpu-migrations。
  - `/home/win11/minidb-final-evidence-v2/perf/software-counters.log`

这些是正向证据，但只覆盖已经实现的路径和已编写测试的矩阵。

## 4. 阶段 A：工程基线

### 4.1 已满足或部分满足

- CMake、CTest、Debug/Release、compile_commands 和 sanitizer 配置可工作。
- 六配置均能构建并通过 152 项测试。
- 已存在 README、文档目录、`.clang-format`、`.clang-tidy`、CMake Presets 和决策记录。
- 基础 RAII、Buffer、Clock、Error 和 fd 测试存在。

### 4.2 阻塞

1. 项目根目录没有 `LICENSE` 或 `COPYING`。`find`、`ls` 和 Git 状态均未发现许可证文件；GitHub API 的 `license` 字段为 `null`。
2. Git 只跟踪 2 个计划文档：
   - `docs/MiniDB-实施计划书.md`
   - `docs/MiniDB-技术评审.md`
3. 实现、测试、构建配置和几乎所有交付文档仍为未跟踪内容，没有可审阅的实现提交。
4. 阶段 A 明确要求初始化许可证并提交一次可审阅提交，这两项均未满足。

**判定：FAIL。**

## 5. 阶段 B：Socket 和非阻塞 I/O 实验

### 5.1 已存在

- `experiments/socket_echo_blocking.cpp`
- `experiments/socket_echo_nonblocking.cpp`
- `experiments/echo_client.py`
- `experiments/CMakeLists.txt`
- `docs/STAGE_B_COMPLETE.md` 声明做过手工 Echo、部分写和 EINTR/EAGAIN 处理。

### 5.2 阻塞

1. `docs/STAGE_B_COMPLETE.md` 声称交付了 `README.md`，但 `experiments/` 下没有该文件。
2. 计划要求保存慢客户端、小发送缓冲、半关闭产生的 strace 片段，以及重复连接和异常断开的 fd 计数记录；这些原始证据没有保留。
3. 阶段完成声明没有可复核的 ASan/UBSan 和 fd 泄漏日志。
4. 仅凭源码和完成摘要，不能证明“稳定复现 EAGAIN”“每个 errno 有处理理由”和“无 fd 泄漏”。

**判定：FAIL（证据不足）。** 不据此断言实验源码不可运行，只认定阶段验收证据不足。

## 6. 阶段 C：LT Reactor 与连接生命周期

### 6.1 已存在

- `src/net/` 包含 Poller、Channel、EventLoop、Acceptor、TcpConnection 等实现。
- 实现中有 accept 循环、EINTR 重试、EMFILE/ENFILE idle fd 处理和 `epoll_ctl` 错误日志。
- 集成测试 `HttpSessionIntegrationTest.PeerHupResetAndEpipeDoNotStopServer` 验证半关闭、RST 后健康请求仍可服务。
- 六配置 152/152，ASan/UBSan/TSan 均无报告。

### 6.2 阻塞

阶段 C 要求独立验证的专项矩阵仍不完整：

- 没有 EMFILE/ENFILE 的故障注入回归，无法证明 idle fd 恢复路径。
- 没有 `epoll_ctl` 失败注入测试。
- 没有证明 EPOLLHUP 到达时先读出内核残余 payload 再回收连接。
- 没有证明 `shutdown(SHUT_WR)` 触发 RDHUP 后已接受响应能够完整 drain。
- fd 重用、对象提前释放和多次 close 的专项回归没有形成完整清单。

现有测试覆盖了部分正常路径和 RST/EPIPE 路径，但不能替代上述门槛。

**判定：FAIL（未完整验收）。**

## 7. 阶段 D：Session 边界和背压

### 7.1 本地证据较强

- `HttpSessionIntegrationTest` 覆盖分片输入、100 次 Keep-Alive、pipelined 顺序、慢读 1 MiB 响应、idle timeout 和异常连接。
- `HttpKeepAliveTest` 覆盖 parser reset、同步写完成安全和粘包解析。
- 状态码、路由和部分资源上限测试存在。

### 7.2 尚未关闭

- 没有直接观测 EPOLLIN 在处理中保持关闭、响应完成后恢复的专项测试。
- stop 幂等、完全资源归还和 output 上限的边界测试没有形成独立验收表。
- 错误响应实际到达客户端后关闭连接的策略矩阵尚未逐项归档。

**判定：未正式关闭，不判 PASS。** 这不否定已有集成测试的价值。

## 8. 阶段 E：HTTP 子集和 HTTP 服务

### 8.1 本地证据较强

`HttpParserP0Test` 与 `HttpSessionIntegrationTest` 覆盖 Host、version、target、重复 Content-Length、CL+TE、414/417/431/501、405 Allow、body 上限、分片输入、慢读和 Keep-Alive。

### 8.2 尚未关闭

- 计划要求“每个字段都能按一字节碎片喂入”，当前只有代表性分片测试，没有逐字段一字节矩阵。
- P0 支持矩阵、错误关闭策略和所有状态码的端到端契约没有形成一份独立逐项验收记录。
- EPOLLIN gating 的可观察行为和 D 阶段相同，仍未直接证明。

**判定：未正式关闭，不判 PASS。**

## 9. 阶段 F：KV、Store 与 TTL

### 9.1 已存在

- KV 帧协议、粘包、坏长度、未知命令、二进制值和大 value 测试存在。
- `KvSessionIntegrationTest.ExpireRejectsNonCanonicalAndOverflowingTtl` 覆盖部分 EXPIRE 输入。
- RDB 测试使用 FrozenClock 验证部分 TTL 保存/恢复行为。
- TSan 对现有 152 项测试全绿。

### 9.2 阻塞

1. 缺少表驱动 TTL 语义矩阵：deadline 前、恰好、之后，SET 是否清 TTL，EXPIRE missing/expired/zero/negative/重复设置，TTL 取整和 DEL 语义没有逐项穷举。
2. 缺少 fake clock 下的时钟跳变策略测试。
3. 缺少 SET/EXPIRE/DEL/scanner 交错和可停止 scanner 的专项测试。
4. 缺少固定 seed 的 model-based/concurrent stress；现有 TSan 全绿不等同于固定 seed stress + TSan。

**判定：FAIL（未完整验收）。**

## 10. 阶段 G：RDB 快照和启动恢复

### 10.1 本地证据较强

- RDB v2 格式、Varint、CRC32C 和网络字节序测试存在。
- 截断、bit flip、尾随字节、重复 key、record count、版本错误和非空 Store 不变性测试存在。
- ProcessIntegrationTest 覆盖损坏 RDB 启动失败和 SAVE 失败退出。
- AI-018 补充真实 varint 溢出和正确 CRC 样本。

### 10.2 阻塞

1. 缺少可注入文件操作故障矩阵：EINTR、短写、ENOSPC、open、close、fsync、rename 和父目录 fsync 失败。
2. 缺少 rename 前后不同失败点的旧快照保留和 `DURABILITY_UNCONFIRMED` 专项验证。
3. 缺少并发 writer + marker key 的 SAVE 线性化压力测试。
4. 缺少各阶段 kill -9 重启矩阵。
5. 缺少在线 SAVE 与启动恢复峰值 RSS 相对冻结预算的测量证据。
6. stop-the-world SAVE barrier 的暂停窗口尚未形成可复核指标。

**判定：FAIL（未完整验收）。**

## 11. 阶段 H：集成 QA、性能、文档和开源贡献

### 11.1 已取得进展

- 本地六配置、fuzz、clang-tidy、clang-format、benchmark、strace 和 perf 限制记录均存在。
- 新增架构、协议、依赖、部署、排障、RDB 格式、工具、bench 和项目复盘文档。
- README 已重写并说明构建、运行、测试和限制。

### 11.2 阻塞

GitHub API 证据：

- 仓库：`https://github.com/TXSAJYDN/MiniDB`
- 公开仓库，默认分支 `main`
- `license: null`
- Actions runs：`total_count=0`
- workflows：`total_count=0`
- issues API：空数组，未发现公开 issue/PR 记录
- 远程仓库根目录只有 `docs`，没有源码、测试、构建文件或 LICENSE

本地 `.github/workflows/ci.yml` 仍为未跟踪文件，GitHub Actions 从未真实运行。阶段 H 要求将任务纳入 CI、固定 seed fuzz smoke、公开 issue/PR 尝试和可重复远程交付；这些均未完成。

perf 硬件计数器不可用是允许记录的限制，但计划要求由 Ubuntu CI 或远程 Linux 补测；该补测没有发生。

**判定：FAIL。**

## 12. 交付状态与安全边界

- 分支：`main`
- HEAD：`e4c8d60`
- 未执行 reset、覆盖式 checkout、clean、删除备份、提交或推送。
- 现有实现和测试仍处于未跟踪状态。
- 没有擅自创建 LICENSE，也没有代替项目所有者选择许可证。
- 没有代替项目所有者创建公开 issue/PR 或推送远程仓库。

## 13. 后续关闭顺序建议

在继续新增功能前，优先关闭以下顺序：

1. 由项目所有者决定许可证并加入 `LICENSE`。
2. 将源码、测试、构建配置和文档形成可审阅提交，并建立真实 CI。
3. 补齐阶段 B/C/F/G 的故障注入与专项矩阵。
4. 对 D/E 的 EPOLLIN gating、逐字段碎片和资源上限形成独立验收记录。
5. 由远程 CI 重跑 sanitizer/fuzz smoke，并完成公开 issue/PR 尝试记录。

只有上述门槛全部关闭后，才能重新声称阶段 A-H 整体通过。