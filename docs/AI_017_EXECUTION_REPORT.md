# AI-017 直接修复执行报告

## 修复内容

- 调整 HttpServer 成员声明顺序，使 TcpServer 在 sessions_、KvStore 和 RdbManager 仍存活时先析构，避免连接关闭回调访问已销毁的 sessions_。
- 调整 TcpServer::remove_connection()，将 connect_destroyed() 放入 owner EventLoop 的 pending queue，避免在 Channel::handle_event() 仍执行时销毁当前 Channel，修复 ASan 报告的 use-after-free。
- 保留 AI-017 已完成的 HTTP keep-alive 粘包复制、非递归调度、header/body 限制分离和 RDB 边界修复。

## 验证

权威树：/home/win11/projects/miniDB
构建目录：/tmp/minidb-ai017-final-{debug,release,ubsan,asan,asan-ubsan}
CTest 测试数：80

- Debug: 80/80
- Release: 80/80
- UBSan: 80/80
- ASan: 80/80
- ASan+UBSan: 80/80

ASan+UBSan 曾在修复前复现 Channel::handle_event() use-after-free；修复后完整 CTest 80/80 通过。修复后的真实 socket 测试 BasicGetRequest、KeepAliveMultipleRequests、PipelinedRequests、ShortHeaderWithLargeBody 均通过。

## 限制

本报告记录的是直接修复和独立复测，不等同于阶段 A-H 全部功能验收。性能、TSan、fuzz 和完整协议覆盖仍未验收。

## 2026-11-05 独立复核补充

- 从当前 Linux 权威树重新配置五个仓库外构建目录，使用当前 GoogleTest 缓存源码，重新构建并运行 CTest。Debug、Release、UBSan、ASan、ASan+UBSan 均为 80/80，退出码 0。
- 联合配置命令使用 `-DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON`。HTTP 集成测试的实际编译命令和 `link.txt` 均同时包含 `-fsanitize=address` 与 `-fsanitize=undefined`。联合 sanitizer 下 HTTP socket 子集再次通过 4/4。
- 独立审查未通过完整 AI-017 验收：现有测试缺少超 64 KiB 未结束 request-line/header 分片及累计多行超限用例；RDB load 测试未使用真实溢出 payload `ff ff ff ff 10` 对非空多键 Store 验证失败原子性。详见 `AI_REVIEW_REPORT.md` 的 2026-11-05 结论。
- 因此本报告中的五配置通过只证明当前测试集通过，不等同 AI-017 全部验收通过；当前项目状态以 `PROJECT_STATE.md` 的 AI-017 FAIL 和 AI-018 任务为准。
