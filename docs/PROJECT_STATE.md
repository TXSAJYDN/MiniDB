# MiniDB 项目状态

- 更新时间：2026-11-06（Asia/Shanghai）
- 权威工作树：`/mnt/e/c++/miniDB`（Windows：`E:\c++\miniDB`）
- 远程仓库：`git@github.com:TXSAJYDN/MiniDB.git`
- 分支：`main`
- 验收代码基线：`9da4b48f5fe78e4c2896ad1ca07464361b27e362`
- 当前状态：**AI-018 PASS；阶段 A-H 技术验收 PASS**
License: DEFERRED BY OWNER

## 1. 当前结论

MiniDB 已完成原计划 P0 阶段的 A-H 技术验收。最终依据不是早期自评，而是：

- 六种配置的全新仓库外构建，每配置 214/214 CTest；
- Debug、ASan、UBSan 三套真实 socket 进程证据；
- Reactor、连接生命周期、Session/背压、HTTP、KV/TTL 和 RDB 专项测试；
- RDB EINTR/短写/ENOSPC/open/fsync/close/rename/dir-fsync 故障注入；
- RDB kill -9 分阶段恢复和 64 MiB 文件/RSS/暂停预算；
- HTTP/KV fuzz 各 20,000 runs、固定 seed、artifact 为空；
- clang-format、clang-tidy、benchmark、strace 和 perf 限制记录；
- 真实 GitHub Actions run 42182440543，9/9 job 成功；
- 对 eclipse-4diac/4diac-forte#1135 的真实公开贡献尝试和失败证据。

统一证据目录：

```text
/home/win11/minidb-ah-final-20261106-v3-evidence
```

详细报告：

- `docs/AI_018_EXECUTION_REPORT.md`
- `docs/A_H_ACCEPTANCE_REPORT.md`

## 2. 阶段状态

| 阶段 | 状态 | 当前依据 |
|---|---|---|
| A 工程基线与工具链 | PASS | 已提交/推送；六配置 214/214；CI 9/9；许可证由所有者暂缓 |
| B Socket 与非阻塞 I/O | PASS | Debug/ASan/UBSan echo，EAGAIN/短写/RST/fd/shutdown 证据 |
| C LT Reactor 与连接生命周期 | PASS | Acceptor/Poller/Channel/TcpConnection 专项矩阵 |
| D Session 边界与背压 | PASS | Keep-Alive、pipeline、慢读、EPOLLIN gating、超时和关闭 |
| E HTTP 子集与服务 | PASS | parser/status/limit 矩阵、真实 socket session、进程持久化测试 |
| F KV、Store 与 TTL | PASS | 表驱动 TTL、墙钟跳变、固定 seed model stress、scanner/lease |
| G RDB 与启动恢复 | PASS | 文件故障注入、kill -9、64 MiB 预算和完整恢复 |
| H 集成 QA、性能、文档与贡献 | PASS | CI、fuzz、静态检查、benchmark、strace、公开贡献真实尝试 |

## 3. 代码与交付状态

验收代码提交：

```text
9da4b48f5fe78e4c2896ad1ca07464361b27e362
fix: close AI-018 acceptance gaps
19 files changed, 1172 insertions(+), 60 deletions(-)
```

该提交已推送至 `origin/main`，仓库跟踪 419 个文件。

GitHub Actions：

- Run ID：`42182440543`
- URL：<https://github.com/TXSAJYDN/MiniDB/actions/runs/42182440543>
- 代码提交：`9da4b48f5fe78e4c2896ad1ca07464361b27e362`
- 结果：Success，9/9 job，2m 54s
- 组成：5 个 build/test matrix、TSan、format、static-analysis、fuzz-smoke
- 备注：存在 `actions/checkout@v4` 的 Node.js 20 deprecation warning，非失败项

工作树只保留两个明确不提交的历史文件：

```text
?? -configure.log
?? docs/AI_013_FINAL_SUMMARY.md
```

禁止清理、覆盖或随意加入这两个文件；它们是保留的历史证据。

## 4. 关键验收数据

### 4.1 六配置矩阵

| 配置 | CTest |
|---|---:|
| Debug | 214/214 |
| Release | 214/214 |
| ASan | 214/214 |
| UBSan | 214/214 |
| ASan+UBSan | 214/214 |
| TSan（`setarch x86_64 -R`） | 214/214 |

六个配置的 configure、build、CTest 退出码均为 0。源码摘要为 `2dba58c83beca499affefa42bcf6b970d2b0c83f01c5bcd6e24de75fc0b73843`，对应随后提交为 `9da4b48` 的工作树内容。

### 4.2 Stage B

Debug / ASan / UBSan 均完成真实 echo：

- accepted：30 / 30 / 30；
- EAGAIN：1073 / 905 / 1585；
- 短写：16 / 18 / 51；
- RST：1 / 1 / 1；
- fd before/after：4/4；
- strace 观测 `accept4`、EAGAIN、`shutdown`、`close`、`sendto`。

### 4.3 Stage G

- RDB 大小：`67,108,864` 字节，恰好 64 MiB；
- SHA-256：`4b399a1decfaa878950fc20ba22d164d1984e170069a783798945d5f06c4f555`；
- 64 keys 完整恢复；
- 在线 SAVE HTTP 200，1265.410 ms；
- 在线 SAVE 峰值 RSS 200.785 MiB；
- 启动恢复峰值 RSS 132.621 MiB；
- 512 MiB RSS 预算、30 秒 SAVE 暂停预算通过；
- 无 `.tmp.*` 残留。

### 4.4 Stage H

- fuzz：HTTP 20,000、KV 20,000，seed 20261105，artifact 0；
- replay：HTTP 427 个输入、KV 42 个输入；
- clang-format：81 文件，退出码 0；
- clang-tidy：25 文件，0 失败；
- benchmark：HTTP 2344.653 req/s、P99 654.924 us；KV 2400.599 req/s、P99 771.885 us；
- perf 硬件计数器不可用，仅记录软件计数器，不声称 cycles/instructions/flamegraph。

## 5. 公开贡献尝试

目标 issue：

- <https://github.com/eclipse-4diac/4diac-forte/issues/1135>

补丁：

- 工作树：`/home/win11/minidb-public-contribution-4diac`
- 基线：`efcd63c957ec191cab4fd57f8c5d6e3a3a30e0e2`
- 提交：`ac80baaf7df26ff1f3ae50d28b19acece4db9ccf`
- 变更：4 files changed，48 insertions(+)
- 内容：新增 `func_MUX.h`、CMake/聚合头更新、五种类型测试、DCO sign-off
- 定向测试通过；完整 CTest 1/1 通过，123.54 秒；clang-format 和 `git diff --check` 通过

推送/PR 结果：

- 上游推送退出 128：`Permission to eclipse-4diac/4diac-forte.git denied to TXSAJYDN`；
- fork 推送退出 128：`Repository not found`；
- fork 公开 API 404；
- 匿名 PR HTTP 401 `Requires authentication`。

结论：**真实尝试完成，但 PR 未创建**。不得写成贡献已提交、已合并或被上游接受，也不得声称 Eclipse ECA 已完成。

## 6. 当前限制

- 许可证：所有者暂缓，未创建 `LICENSE`/`COPYING`；报告头的许可证字段不是许可证文本。
- 公开贡献：补丁已本地验证，但无 fork 权限且无认证，未创建 PR。
- 性能：硬件计数器在 WSL2 不可用；benchmark 是单连接 loopback 数据，不代表饱和吞吐或生产 SLA。
- 环境：主要在 Ubuntu 24.04 WSL2 验证；其他 Linux 环境需重新验证 TSan 和性能。
- 功能：无 TLS、认证、授权、复制、集群、分片、事务、多进程 RDB 文件锁。
- 结论：A-H 技术验收通过不等于生产就绪。

## 7. 后续工作

原计划 P0/A-H 已完成，没有剩余技术验收阻塞。若项目继续推进，建议按以下顺序：

1. 项目所有者确定许可证后再补 `LICENSE`，不要在未授权时自行选择。
2. 配置可写的 GitHub fork 和认证，重新尝试 4diac PR，并完成 Eclipse ECA。
3. 若追求生产化，另行规划 TLS/认证、复制与一致性、多进程所有权、长稳/压力/多客户端基准和跨平台验证。
4. 保持现有六配置、fuzz、故障注入和 CI 门禁，不要用单次 happy-path 测试替代验收。

历史审查过程保留在 `docs/AI_REVIEW_REPORT.md` 和 AI-011 至 AI-018 的执行报告中；本文件只记录当前权威状态，早期 FAIL 结论已被本轮修复和验收关闭。