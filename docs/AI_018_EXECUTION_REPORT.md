# MiniDB AI-018 执行与独立验收报告

- 报告日期：2026-11-06（Asia/Shanghai）
- 权威工作树：`/mnt/e/c++/miniDB`（Windows：`E:\c++\miniDB`）
- 远程仓库：`git@github.com:TXSAJYDN/MiniDB.git`
- 分支：`main`
- 验收代码基线：`9da4b48f5fe78e4c2896ad1ca07464361b27e362`
- 环境：Ubuntu 24.04 WSL2 ext4，GCC 13.3.0，CMake 3.28.3，Clang 18.1.3
- 任务范围：AI-017 独立验收指出的 HTTP 超限回归与 RDB 真实 varint 溢出回归，并完成阶段 A-H 技术验收
- 结论：**AI-018 PASS；阶段 A-H 技术验收 PASS**
License: DEFERRED BY OWNER

## 1. 结论先行

AI-018 的两类证据缺口已经关闭，六种配置的完整矩阵均为 214/214：

1. HTTP parser 对没有 CRLF 的超长 request-line/header 分片立即映射为 414/431，并拒绝多条各自合法但累计超限的 header。
2. RDB load 拒绝真实第五字节 payload 溢出的 varint；在非空 Store 上失败时不发布部分结果，既有 value、二进制值和 TTL 保持不变。

在 AI-018 之后，阶段 A-H 的剩余验收项也已完成复核：

- 六配置矩阵：Debug、Release、ASan、UBSan、ASan+UBSan、TSan，每项 configure/build/CTest 退出码均为 0，均为 214/214。
- Stage B：Debug、ASan、UBSan 三套真实 socket echo/syscall 证据通过。
- Stage C/D/E/F/G：错误注入、生命周期、背压、HTTP 契约、TTL model stress、RDB 故障与 kill -9 恢复证据均已进入 CTest 并获得通过结果。
- Stage H：真实 GitHub Actions run 42182440543 的 9 个 job 全部成功；fuzz、静态检查、格式、benchmark、strace 和公开贡献尝试均有可复核记录。
- License：项目所有者决定暂缓许可证选择，因此交付物只在报告头记录所有者的许可证暂缓决定，不创建 `LICENSE`/`COPYING`。

`A-H PASS` 表示原计划的 P0 技术验收门槛通过，不表示 MiniDB 已生产就绪。TLS、认证、复制、集群、事务、多进程文件锁等能力仍明确不在当前范围内。

## 2. 提交与源码快照

验收代码提交：

```text
9da4b48f5fe78e4c2896ad1ca07464361b27e362
fix: close AI-018 acceptance gaps
19 files changed, 1172 insertions(+), 60 deletions(-)
```

该提交包含 HttpServer/KvServer 生命周期和 TTL/RDB 故障边界修复、固定 seed TTL stress、RDB 故障注入、Stage G 验收脚本、六配置矩阵工具和 CI 格式范围修正。提交已推送到 `origin/main`。

矩阵采集时的 Git HEAD 为 `0bf20e1c45eada3b4ee8f80ddb70c6e9454ef2a6`，工作树包含随后原样提交为 `9da4b48` 的 19 个文件变更。矩阵证据记录：

| 项目 | 值 |
|---|---|
| 源码路径 | `/mnt/e/c++/miniDB` |
| 源码摘要 | `2dba58c83beca499affefa42bcf6b970d2b0c83f01c5bcd6e24de75fc0b73843` |
| 快照文件数 | 653 |
| 六配置构建根目录 | `/home/win11/minidb-ah-final-20261106-v3-matrix` |
| 统一证据目录 | `/home/win11/minidb-ah-final-20261106-v3-evidence` |

工作树内容在矩阵之后没有源码改动；后续变更仅限交付文档。这个顺序保证矩阵结果能够对应到已推送的 `9da4b48` 代码内容，而不是一个无法追溯的本地副本。

## 3. HTTP 超限专项回归

P0 线协议上限来自 `src/protocol/HttpParser.h`：

- `kMaxRequestLineSize = 4096` 字节
- `kMaxHeaderSize = 16384` 字节
- `kMaxHeaderFieldCount = 64`
- `kMaxBodySize = 1048576` 字节

新增或确认的测试在全部六种配置中通过：

| 编号 | 测试 | 输入与断言 | 结果 |
|---:|---|---|---|
| 48 | `HttpParserP0Test.AccumulatedCompleteHeadersOverLimitMapsTo431` | 先消费合法 request-line，再追加三个各 6012 字节的完整 header；前两行累计 12024 字节，第三行后累计 18036 字节。 | 前两次等待更多数据，第三次返回 `kHeaderTooLarge`，映射 431。 |
| 52 | `HttpParserP0Test.UnterminatedRequestLineBeyond64KiBIsRejectedAs414` | 一次性追加 65537 个 `R`，不存在 CRLF。 | 比较 4096 字节上限后立即失败，错误为 `kUriTooLong`，映射 414。 |
| 53 | `HttpParserP0Test.UnterminatedHeaderBeyond64KiBIsRejectedAs431` | 消费 `GET / HTTP/1.1\r\n` 后追加 65537 个 `v`，不存在 CRLF。 | 比较 16384 字节上限后立即失败，错误为 `kHeaderTooLarge`，映射 431。 |
| 82 | `RdbManagerTest.Load_RealVarintOverflowPreservesNonEmptyStore` | 使用合法 v2 文件结构、正确 CRC32C、真实第五字节 payload 溢出和非空目标 Store。 | `load()` 返回 -1，目标 Store 的键、值、二进制值和 TTL 全部保持不变。 |

Debug 配置的关键输出：

```text
1/4 Test #48: HttpParserP0Test.AccumulatedCompleteHeadersOverLimitMapsTo431 ... Passed
2/4 Test #52: HttpParserP0Test.UnterminatedRequestLineBeyond64KiBIsRejectedAs414 ... Passed
3/4 Test #53: HttpParserP0Test.UnterminatedHeaderBeyond64KiBIsRejectedAs431 ... Passed
4/4 Test #82: RdbManagerTest.Load_RealVarintOverflowPreservesNonEmptyStore ... Passed

100% tests passed, 0 tests failed out of 4
```

## 4. RDB 真实溢出样本与失败原子性

`RdbManagerTest.Load_RealVarintOverflowPreservesNonEmptyStore` 使用辅助函数生成 v2 header 和 CRC32C。校验范围内的 header+record 共 35 字节，完整文件为 39 字节：

```text
4d 52 44 42  00 00 00 02  00 00 00 01
ff ff ff ff 10
62 61 64
06 76 61 6c 75 65 31
00 00 00 00 00 00 00 00
00 0f 14 c1
```

关键字段：

| 字节 | 字段 | 说明 |
|---|---|---|
| `4d 52 44 42` | magic | `MRDB` |
| `00 00 00 02` | version | v2，网络字节序 |
| `00 00 00 01` | record count | 1 条记录 |
| `ff ff ff ff 10` | key length varint | 第五字节无 continuation bit，但 payload `0x10 > 0x0f`，是真实 uint32 溢出 |
| `62 61 64` | key | `bad` |
| `06 76 61 6c 75 65 31` | value | 长度 6，内容 `value1` |
| `00 00 00 00 00 00 00 00` | expiry | 0，无 TTL |
| `00 0f 14 c1` | CRC32C | 覆盖前 35 字节 |

CRC32C 由独立 Python 反射 Castagnoli 实现复算为 `0x000f14c1`。记录其余字段完整且 checksum 正确，因此失败原因只能是 varint 溢出，不是截断或错误 checksum。

目标 Store 在 load 前预置：

- `alpha=value-alpha`，无 TTL；
- `beta=bin\0ary\xff`，TTL 60000 ms；
- `gamma=value-gamma`，无 TTL。

失败后逐项断言：Store 大小不变；三个既有 key 的 value 不变；`bad` 未写入；`alpha`、`gamma` 仍无 TTL；`beta` 仍为 expiring 且剩余 TTL 精确等于原值。该测试证明失败路径没有发布部分 Store。

## 5. 六配置完整矩阵

所有配置均使用仓库外全新构建目录，configure、build、CTest 三个退出码均为 0：

| 配置 | 关键参数 | configure | build | CTest | 结果 |
|---|---|---:|---:|---:|---:|
| Debug | `CMAKE_BUILD_TYPE=Debug` | 0 | 0 | 0 | 214/214 |
| Release | `CMAKE_BUILD_TYPE=Release` | 0 | 0 | 0 | 214/214 |
| ASan | `ENABLE_ASAN=ON` | 0 | 0 | 0 | 214/214 |
| UBSan | `ENABLE_UBSAN=ON` | 0 | 0 | 0 | 214/214 |
| ASan+UBSan | `ENABLE_ASAN=ON ENABLE_UBSAN=ON` | 0 | 0 | 0 | 214/214 |
| TSan | `ENABLE_TSAN=ON` | 0 | 0 | 0 | 214/214 |

TSan 使用 `setarch x86_64 -R` 运行，以避开当前 WSL2 地址空间布局对 TSan 的干扰。汇总和日志：

- `/home/win11/minidb-ah-final-20261106-v3-evidence/matrix-summary.json`
- `/home/win11/minidb-ah-final-20261106-v3-evidence/{debug,release,asan,ubsan,asan-ubsan,tsan}/{configure,build,ctest}.log`

ASan+UBSan 的 `compile_commands.json` 中 `-fsanitize=address` 和 `-fsanitize=undefined` 各出现 55 次；`base_test` 与 `storage_test` 的 `link.txt` 中两种标志各出现 2 次。TSan 的编译标志出现 55 次，测试链接命令包含 `-fsanitize=thread`。六配置均未出现 sanitizer 诊断。

## 6. Fuzz、静态检查、格式与性能

### 6.1 Fuzz

Stage H 证据 `/home/win11/minidb-ah-final-20261106-v3-evidence/stage-h/stage-h-summary.json` 记录：

- seed：`20261105`；
- HTTP：20,000 runs，`max_len=2097152`，退出码 0；
- KV：20,000 runs，`max_len=1179648`，退出码 0；
- sanitizer：AddressSanitizer + UndefinedBehaviorSanitizer；
- HTTP replay 输入 427 个文件，KV replay 输入 42 个文件，退出码均为 0；
- 所有 fuzz/replay 结束后 artifact 数量为 0，源 corpus 未被修改。

### 6.2 静态检查和格式

- clang-format 18.1.3：81 个文件，退出码 0；
- clang-tidy 18.1.3：25 个源文件，失败 0，退出码 0。

证据位于 Stage H 证据目录的 `clang-format-provenance.txt`、`clang-format.log` 和 `clang-tidy-summary.txt`。

### 6.3 Release benchmark

每协议 200 次 warmup、2000 次测量：

| 协议 | 吞吐 | P50 | P95 | P99 |
|---|---:|---:|---:|---:|
| HTTP | 2344.653 req/s | 422.208 us | 563.464 us | 654.924 us |
| KV | 2400.599 req/s | 405.135 us | 627.115 us | 771.885 us |

这是单连接、loopback、WSL2 环境数据，不代表饱和吞吐或多客户端扩展性。原始 JSON 为 `stage-h/release-wsl2.json`。

### 6.4 syscall、strace 与 perf 限制

strace 观测到 `accept4`、`epoll_wait`、`epoll_ctl`、`read`、`write`、`openat`、`fsync`、`rename`、`close`，并记录了 EAGAIN。证据：

- `/home/win11/minidb-ah-final-20261106-v3-evidence/stage-h/strace/strace-summary.txt`
- `/home/win11/minidb-ah-final-20261106-v3-evidence/stage-h/strace/strace.log`

perf 硬件计数器在当前 WSL2 不可用：

```text
No supported events found.
The cycles:u event is not supported.
```

因此本报告不声称任何 cycles、instructions、branches 或 cache-misses 数据。软件计数器运行成功，记录 `task-clock`、`page-faults`、`context-switches` 和 `cpu-migrations`；证据为 `stage-h/perf-results.txt` 与 `stage-h/perf-software.csv`。

## 7. Git、CI 与公开贡献

### 7.1 Git 交付

- `main` 已推送到 `origin/main`；
- 验收代码基线为 `9da4b48`；
- 仓库当前跟踪 419 个文件，包含源码、测试、构建配置、CI 和文档；
- 工作树中只保留两个明确不提交历史文件：`-configure.log`、`docs/AI_013_FINAL_SUMMARY.md`。

### 7.2 GitHub Actions

真实 push 触发 run：

- Run ID：`42182440543`
- URL：<https://github.com/TXSAJYDN/MiniDB/actions/runs/42182440543>
- 代码提交：`9da4b48f5fe78e4c2896ad1ca07464361b27e362`
- 结论：Success
- 用时：2m 54s
- 结果：9/9 job 成功

9 个 job 由一个 5 项 build/test matrix、TSan、format、static-analysis 和 fuzz-smoke 组成。运行页面存在 `actions/checkout@v4` 使用 Node.js 20 的 deprecation warning；这是非失败警告，不应写成 CI 失败。

### 7.3 公开贡献尝试

目标 issue：

- <https://github.com/eclipse-4diac/4diac-forte/issues/1135>
- 标题：`no func_MUX.h, although the 4diac IDE 3.3.0 exports an include for it`

在独立工作树 `/home/win11/minidb-public-contribution-4diac` 基于上游 `efcd63c957ec191cab4fd57f8c5d6e3a3a30e0e2` 实现了补丁：

- 提交：`ac80baaf7df26ff1f3ae50d28b19acece4db9ccf`
- 提交信息：`Add missing func_MUX header`
- 变更：4 files changed, 48 insertions(+)
- 内容：新增 `core/include/forte/iec61131_functions/func_MUX.h`，更新 CMake 头文件列表和聚合头，增加 INT/UINT/BYTE/BOOL/TIME 测试，包含 DCO sign-off。

验证结果：

- 定向 `IEC61131_functions/func_mux` 测试通过；
- 完整 CTest `1/1` 通过，退出码 0，用时 123.54 秒；
- clang-format 通过；
- `git diff --check` 通过。

真实推送和 PR 尝试均失败，不能写成“已创建 PR”：

- 上游推送退出 128：`Permission to eclipse-4diac/4diac-forte.git denied to TXSAJYDN`；
- 用户 fork 推送退出 128：`Repository not found`；
- `TXSAJYDN/4diac-forte` 公开 API 返回 404；
- 匿名 PR 创建返回 HTTP 401 `Requires authentication`。

`CONTRIBUTING.md` 要求 Eclipse ECA，本报告不声称用户已完成 ECA。证据位于 `/home/win11/minidb-ah-final-20261106-v3-evidence/public-contribution/`。

## 8. 最终结论

**AI-018：PASS。**

- 四项专项回归在六种配置中均通过；
- 六种配置的完整 CTest 均为 214/214；
- HTTP 的 414/431 边界和累计 header 超限有真实输入断言；
- RDB 使用正确 v2 格式、正确 CRC32C `0x000f14c1` 和真实第五字节 varint 溢出，并证明非空 Store 失败不变性；
- fuzz、静态检查、格式检查、benchmark、strace、perf 限制和 CI 证据可复核。

**MiniDB 阶段 A-H：PASS。**

原计划的阶段门槛已经关闭。许可证决定被所有者明确暂缓，不作为技术失败；perf 硬件计数器不可用和公开贡献因认证/fork 缺失未能创建 PR，均作为真实限制记录。项目仍非生产就绪，详见 `docs/A_H_ACCEPTANCE_REPORT.md`。