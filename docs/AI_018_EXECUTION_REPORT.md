# MiniDB AI-018 执行与独立验收报告

- 报告日期：2026-11-06（Asia/Shanghai）
- 矩阵证据采集：2026-11-05
- 权威工作树：`/home/win11/projects/miniDB`
- 分支/HEAD：`main` / `e4c8d60`
- 环境：Ubuntu 24.04 WSL2 ext4，GCC 13.3.0，CMake 3.28.3，Clang 18.1.3
- 任务范围：AI-017 独立验收指出的 HTTP 超限回归与 RDB 真实 varint 溢出回归
- 验收结论：**AI-018 PASS**
- 项目阶段结论：**阶段 A-H 整体 FAIL**，详见 `docs/A_H_ACCEPTANCE_REPORT.md`

> AI-018 的 PASS 只表示本轮四项专项回归、六配置构建和测试矩阵及配套验证达到任务门槛。它不表示许可证、Git 交付、CI、公开贡献或阶段 A-H 的其他专项门槛已经通过。

## 1. 任务与改动边界

AI-018 要关闭的两类证据缺口是：

1. HTTP parser 需要证明无 CRLF 的超长 request-line/header 分片会立即映射为 414/431，且多条各自合法但累计超限的 header 也会被拒绝。
2. RDB load 需要证明真实第五字节 varint 溢出会被拒绝，并且在非空 Store 上失败时不会发布部分结果或改变既有值、TTL。

本轮可归属变更为：

- `tests/HttpParserP0Test.cpp`
  - `HttpParserP0Test.UnterminatedRequestLineBeyond64KiBIsRejectedAs414`
  - `HttpParserP0Test.UnterminatedHeaderBeyond64KiBIsRejectedAs431`
  - `HttpParserP0Test.AccumulatedCompleteHeadersOverLimitMapsTo431`
- `tests/RdbManagerTest.cpp`
  - `RdbManagerTest.Load_RealVarintOverflowPreservesNonEmptyStore`
- `docs/AI_018_EXECUTION_REPORT.md`
  - 由本报告替换旧版五配置、旧 CRC 和旧测试编号记录。

未修改 `src/` 生产代码、测试目标注册或构建参数；未执行提交、推送或 Git 历史改写。

## 2. 源码快照与复现信息

| 项目 | 值 |
|---|---|
| 权威源码 | `/home/win11/projects/miniDB` |
| Git 分支 | `main` |
| Git HEAD | `e4c8d60` |
| 矩阵快照 `source_sha256` | `2d30253146a4198b64bd7a5de36aa21fc10b4319f2991fe563f2c42699414593` |
| 快照文件数 | 102 |
| 六配置构建根目录 | `/home/win11/minidb-final-matrix-v2` |
| 最终证据目录 | `/home/win11/minidb-final-evidence-v2` |

`source_sha256` 对应矩阵运行时的源码/测试快照。之后仅新增或同步交付文档，因此当前完整工作树的摘要会不同；本报告不把后续文档变更混入矩阵源码快照。

## 3. HTTP 超限专项回归

当前 P0 线协议常量来自 `src/protocol/HttpParser.h`：

- `kMaxRequestLineSize = 4096` 字节
- `kMaxHeaderSize = 16384` 字节
- `kMaxHeaderFieldCount = 64`
- `kMaxBodySize = 1048576` 字节

四项专项测试在六种配置中的 CTest 编号一致：

| 编号 | 测试 | 输入与断言 | 期望结果 |
|---:|---|---|---|
| 48 | `HttpParserP0Test.AccumulatedCompleteHeadersOverLimitMapsTo431` | 先消费合法 request-line；再逐行追加 3 个 `X-Fill-N: <6000 bytes>\r\n`。每行 6012 字节，前两行累计 12024 字节，第三行后累计 18036 字节。 | 前两次解析等待更多数据；第三次返回 `kHeaderTooLarge`，即 431。 |
| 52 | `HttpParserP0Test.UnterminatedRequestLineBeyond64KiBIsRejectedAs414` | 一次性追加 65537 个 `R`，不存在 CRLF，远高于 4096 字节 request-line 上限。 | `parse_request()` 返回 false，`has_error()==true`，错误为 `kUriTooLong`，即 414。 |
| 53 | `HttpParserP0Test.UnterminatedHeaderBeyond64KiBIsRejectedAs431` | 先成功消费 `GET / HTTP/1.1\r\n`，再追加 65537 个 `v` 且不存在 CRLF，远高于 16384 字节 header 上限。 | 剩余 header 分片立即失败，错误为 `kHeaderTooLarge`，即 431。 |
| 82 | `RdbManagerTest.Load_RealVarintOverflowPreservesNonEmptyStore` | 使用 v2 RDB、正确 CRC、真实第五字节 payload 溢出和非空目标 Store。 | `load()` 返回 -1，目标 Store 的键、二进制值和 TTL 全部保持不变。 |

测试名称中的 `Beyond64KiB` 描述的是输入规模远超 64 KiB；实际常量上限分别更小，因此断言的是生产代码的 4 KiB request-line 与 16 KiB header 契约。

## 4. RDB 真实溢出样本与原子性

`RdbManagerTest.Load_RealVarintOverflowPreservesNonEmptyStore` 使用辅助函数生成 v2 header 和 CRC32C。校验范围内的 header+record 共 35 字节，完整文件为 39 字节：

```text
4d 52 44 42  00 00 00 02  00 00 00 01
ff ff ff ff 10
62 61 64
06 76 61 6c 75 65 31
00 00 00 00 00 00 00 00
00 0f 14 c1
```

字段解释：

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

CRC32C 已用独立 Python 反射 Castagnoli 实现复算，结果为 `0x000f14c1`。记录其余字段完整且 checksum 正确，因此失败原因只能是 varint 溢出，不是截断或坏 checksum。

目标 Store 在 load 前预置：

- `alpha=value-alpha`，无 TTL
- `beta=bin\0ary\xff`（二进制值），TTL 60000 ms
- `gamma=value-gamma`，无 TTL

load 失败后逐项断言：Store 大小不变；三个既有 key 的 value 不变；`bad` 未写入；`alpha`、`gamma` 仍无 TTL；`beta` 仍为 expiring 且剩余 TTL 精确等于原值。这证明失败路径没有发布部分 Store。

## 5. 六配置完整矩阵

所有配置均使用仓库外构建目录，configure、build、CTest 三个退出码均为 0：

| 配置 | 关键参数 | configure | build | CTest | 结果 |
|---|---|---:|---:|---:|---:|
| Debug | `CMAKE_BUILD_TYPE=Debug` | 0 | 0 | 0 | 152/152 |
| Release | `CMAKE_BUILD_TYPE=Release` | 0 | 0 | 0 | 152/152 |
| ASan | `ENABLE_ASAN=ON` | 0 | 0 | 0 | 152/152 |
| UBSan | `ENABLE_UBSAN=ON` | 0 | 0 | 0 | 152/152 |
| ASan+UBSan | `ENABLE_ASAN=ON ENABLE_UBSAN=ON` | 0 | 0 | 0 | 152/152 |
| TSan | `ENABLE_TSAN=ON` | 0 | 0 | 0 | 152/152 |

TSan 使用 `setarch x86_64 -R` 运行，以避开当前 WSL2 地址空间布局对 TSan 的干扰。汇总文件：

- `/home/win11/minidb-final-evidence-v2/matrix-summary.json`
- `/home/win11/minidb-final-evidence-v2/targeted/summary.json`
- `/home/win11/minidb-final-evidence-v2/{debug,release,asan,ubsan,asan-ubsan,tsan}/{configure,build,ctest}.log`
- `/home/win11/minidb-final-evidence-v2/targeted/{debug,release,asan,ubsan,asan-ubsan,tsan}.log`

四项专项测试在六种配置中均为 4/4、退出码 0。Debug 目标日志显示编号 48、52、53、82 全部 Passed：

```text
1/4 Test #48: HttpParserP0Test.AccumulatedCompleteHeadersOverLimitMapsTo431 ... Passed
2/4 Test #52: HttpParserP0Test.UnterminatedRequestLineBeyond64KiBIsRejectedAs414 ... Passed
3/4 Test #53: HttpParserP0Test.UnterminatedHeaderBeyond64KiBIsRejectedAs431 ... Passed
4/4 Test #82: RdbManagerTest.Load_RealVarintOverflowPreservesNonEmptyStore ... Passed

100% tests passed, 0 tests failed out of 4
```

## 6. Sanitizer 编译与链接证据

ASan+UBSan 构建目录：

- `compile_commands.json` 中 `-fsanitize=address` 出现 48 次，`-fsanitize=undefined` 出现 48 次。
- `base_test` 与 `storage_test` 的 `link.txt` 中，两种 sanitizer 标志各出现 2 次（公共编译 flags 与链接 flags）。
- 完整 CTest 152/152，无 ASan/UBSan 报告。

TSan 构建目录：

- `compile_commands.json` 中 `-fsanitize=thread` 出现 48 次。
- `base_test` 与 `storage_test` 的 `link.txt` 中各出现 2 次。
- 使用 `setarch x86_64 -R` 的完整 CTest 152/152，无 TSan 报告。

## 7. Fuzz、静态检查、格式与性能证据

### 7.1 Fuzz

`/home/win11/minidb-fuzz-evidence-v2/summary.json` 记录：

- seed：`20261105`
- HTTP：20,000 runs，退出码 0，artifact 为空
- KV：20,000 runs，退出码 0，artifact 为空
- 所有运行结束后 artifact 目录仍为空

### 7.2 静态检查和格式

- clang-tidy：`FILES_CHECKED=25`、`FAILED_FILES=0`、`CLANG_TIDY_EXIT=0`
  - `/home/win11/minidb-static-evidence-v2/clang-tidy-v2.log`
- clang-format：`FORMAT_EXIT=0`
  - `/home/win11/minidb-final-evidence-v2/clang-format-20261106.log`

### 7.3 Release benchmark

最终 benchmark 使用每协议 200 次 warmup、2000 次测量和 2000 个原始延迟样本：

| 协议 | 吞吐 | P50 | P95 | P99 |
|---|---:|---:|---:|---:|
| HTTP | 2611.453 req/s | 360.382 us | 543.037 us | 656.486 us |
| KV | 2662.157 req/s | 355.550 us | 515.647 us | 625.227 us |

证据：`/home/win11/minidb-final-evidence-v2/benchmark/release-wsl2-20261105.json`。这些是单连接、loopback、WSL2 环境数据，不代表饱和吞吐或多客户端扩展性。

### 7.4 syscall 与 perf 限制

strace 观测到 `accept4`、`epoll_wait`、`epoll_ctl`、`read`、`write`、`openat`、`fsync`、`rename`、`close` 等预期调用。证据：

- `/home/win11/minidb-final-evidence-v2/strace/strace-summary.txt`
- `/home/win11/minidb-final-evidence-v2/strace/strace.log`

perf 硬件计数器在当前 WSL2 不可用：

```text
No supported events found.
The cycles:u event is not supported.
```

因此本报告不声称任何 cycles、instructions、branches 或 cache-misses 数据。软件计数器运行成功：`task-clock:u=544309872`、`page-faults:u=2869`、`context-switches:u=0`、`cpu-migrations:u=0`。证据：`/home/win11/minidb-final-evidence-v2/perf/software-counters.log`。

## 8. Git 与交付状态

- 分支：`main`
- HEAD：`e4c8d60`
- `git ls-files` 仍只有 2 个计划文档；实现、测试和多数文档均未跟踪。
- 项目根目录没有 `LICENSE` 或 `COPYING`；GitHub API 的 `license` 也是 `null`。
- 本轮未执行 reset、覆盖式 checkout、clean、删除备份、提交或推送。

这些是阶段 A 和阶段 H 的交付阻塞，不影响本轮四项专项测试的事实判断，但禁止把 AI-018 PASS 扩大为项目整体通过。

## 9. 最终结论

**AI-018：PASS。**

- 四项专项回归在六种配置中均为 4/4。
- 六种配置的完整 CTest 均为 152/152。
- HTTP 的 414/431 边界和累计 header 超限有真实输入断言。
- RDB 使用正确 v2 格式、正确 CRC `0x000f14c1` 和真实 varint 溢出，并证明非空 Store 失败不变性。
- Fuzz、静态检查、格式检查、benchmark、strace 和 sanitizer 构建/链接证据可复核。

**MiniDB 阶段 A-H：FAIL。**

本地实现和测试覆盖已经较强，但许可证、可审阅提交、CI 实际运行、公开 issue/PR 以及阶段 B/C/F/G 的专项门槛仍未全部关闭。完整判定见 `docs/A_H_ACCEPTANCE_REPORT.md`。