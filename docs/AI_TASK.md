# MiniDB AI-018 开发任务

- 任务编号：AI-018
- 任务类型：AI-017 独立验收失败后的最小测试证据补齐
- 执行代理：Claude Opus 5.5
- 状态：待执行
- 权威工作树：/home/win11/projects/miniDB
- 前置条件：AI-017 的生命周期修复已通过五种配置 80/80 回归，但完整验收 FAIL，原因仅限专项测试证据缺失。

## 项目目标及架构约定

继续使用现有自主实现的 EventLoop、epoll、TcpConnection、HTTP parser/session 和 RDB 格式。不得引入成熟网络库，不得把测试缺口改写为架构通过。AI-018 只补测试和必要的最小测试辅助代码，不扩展新功能。

## 唯一主要目标

补齐并独立验证以下两类回归：

1. HTTP parser 对超过 64 KiB 且尚未出现 CRLF 的 request-line/header 分片，以及累计多行 header 超限，必须及时拒绝。
2. RDB load 对真实第五字节 payload 大于 0x0f 的 varint 溢出样本必须拒绝；该失败必须在已有多个 key（含 TTL/值）的目标 Store 上验证原 Store 完整不变。

## 当前已确认前置条件

- Linux 权威树为 /home/win11/projects/miniDB，Ubuntu 24.04 WSL2 ext4。
- AI-017 生命周期修复已在 Debug、Release、UBSan、ASan、ASan+UBSan 五种配置各通过 80/80。
- 当前源码、测试和文档均为未跟踪文件，禁止清理、覆盖、reset、checkout 或提交。
- 当前已有 RdbFormat 解码器单测 ff ff ff ff 10 溢出拒绝，但缺少经 RdbManager::load 的真实文件路径验证。
- 当前已有 HttpParser 分片检查实现，但缺少对应超限分片测试。

## 详细实施步骤

1. 在 tests/HttpParserP0Test.cpp 增加未结束 request-line 超过 64 KiB 的分片测试；增加未结束 header 行超过 64 KiB 的分片测试；增加多次追加、每次低于限制但累计超过限制的 header 测试。断言 parse_request() 返回 false 或 parser 进入错误状态。
2. 在 tests/RdbManagerTest.cpp 或 tests/RdbManagerP0Test.cpp 构造合法 RDB header、真实第五字节 payload 0x10 的 key length varint，并附加正确 CRC。目标 Store 预先写入至少三个 key，至少一个带 TTL；调用 load() 后断言返回 -1，所有 key/value/TTL 仍与加载前一致，不能使用空 Store。
3. 测试构造必须使用已有 RdbFormat::crc32c 和网络字节序接口，避免只靠错误 checksum 触发失败。
4. 仅在测试编译接口需要时修改测试 CMake；不得修改生产代码，除非测试证明现有行为确实错误且修改范围可证明最小必要变更。
5. 在仓库外新目录重新配置、构建并运行 Debug、Release、ASan、UBSan、ASan+UBSan；CTest 必须显示新增测试且全部通过。联合 sanitizer 必须检查编译和链接命令同时包含两种 sanitizer。
6. 生成 docs/AI_018_EXECUTION_REPORT.md，记录测试新增、构造样本、五配置命令/退出码/测试数量、sanitizer 标志和未执行项。

## 允许修改范围

- tests/HttpParserP0Test.cpp
- tests/RdbManagerTest.cpp 或 tests/RdbManagerP0Test.cpp
- tests/CMakeLists.txt
- docs/AI_018_EXECUTION_REPORT.md
- 必要时更新 docs/AI_EXECUTION_REPORT.md

## 禁止修改范围

- 禁止修改 src/生产代码，除非新增回归测试在现有正确性基础上证明生产代码错误且修复是最小必要变更。
- 禁止修改计划书、技术评审、历史审查结论、Windows 9p 副本或 Git 历史。
- 禁止 reset、checkout 覆盖、clean、强制推送、自动提交。
- 禁止引入成熟网络库或扩大为新模块开发。

## 异常与边界条件

- 未结束 request-line/header 测试必须让输入实际超过 65536 字节，不能包含 CRLF，否则不能证明分片路径。
- 累计多行测试必须证明每个单独分片未超过限制，但累计结果超过限制。
- RDB 溢出测试必须使用第五字节 payload 0x10 或更大且无 continuation bit；ff ff ff ff 0f 是合法最大值，不得作为溢出样本。
- CRC 必须覆盖完整 header+record 区域，损坏文件的 CRC 本身应正确，失败原因应是格式/长度溢出。
- Store 不变性至少逐项检查多个 key、多个 value 和 TTL 语义。

## 验收标准

- 新增 HTTP 分片/累计超限测试全部进入 CTest 并通过。
- 新增 RDB load 溢出/Store 不变性测试进入 CTest 并通过。
- 五种配置全新构建、CTest 全部成功，测试数量大于当前 80 项且无失败。
- 联合 sanitizer 的 compile/link 命令同时出现 -fsanitize=address 和 -fsanitize=undefined。
- 报告提供每种配置的 configure/build/CTest 退出码、测试数、关键输出、最终 Git 状态；不得自行宣称阶段 A-H 全部通过。

## 未完成处理

任一测试失败、未进入 CTest、CRC 不正确、Store 不变性断言不足或 sanitizer 证据缺失，都保持 AI-018 FAIL，保留已有通过结果并记录失败原因。