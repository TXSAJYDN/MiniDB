# Stage B：Socket Echo 与非阻塞 I/O 验收完成

- 验收日期：2026-11-06
- 代码基线：`9da4b48f5fe78e4c2896ad1ca07464361b27e362`
- 阶段状态：**PASS**
License: DEFERRED BY OWNER

## 1. 交付内容

阶段 B 的 socket 实验和客户端已进入已跟踪仓库：

- `experiments/socket_echo_blocking.cpp`
  - 阻塞 TCP echo；
  - EINTR、EPIPE、EOF 和 shutdown 处理；
  - `SO_REUSEADDR`。
- `experiments/socket_echo_nonblocking.cpp`
  - 非阻塞 TCP echo；
  - EAGAIN/EWOULDBLOCK；
  - 短写循环；
  - polling loop。
- `experiments/echo_client.py`
  - 基本 echo、并发连接、大量数据和部分写测试。
- `experiments/README.md`
  - 用法、syscall 和 TCP 行为说明。

## 2. 真实进程证据

验收脚本在 Debug、ASan、UBSan 三种构建上启动真实 server/client，而不是仅调用单元函数。

| 配置 | accepted | EAGAIN | 短写 | RST | fd before/after | 客户端退出码 |
|---|---:|---:|---:|---:|---|---:|
| Debug | 30 | 1073 | 16 | 1 | 4/4 | 0 |
| ASan | 30 | 905 | 18 | 1 | 4/4 | 0 |
| UBSan | 30 | 1585 | 51 | 1 | 4/4 | 0 |

ASan/UBSan 未产生 sanitizer 诊断，运行前后 fd 数量一致。

strace 捕获到：

- `accept4`
- EAGAIN
- `shutdown`
- `close`
- `sendto`

## 3. 已验证语义

- TCP 是字节流，写边界不等于读边界。
- 非阻塞 `write`/`send` 可能短写，必须按返回值推进。
- EAGAIN 表示暂时不可写，不应视为连接失败。
- EINTR 应按策略重试。
- EPIPE/RST 表示对端异常终止，不能停止整个 server。
- EOF 表示对端关闭写方向。
- `shutdown(SHUT_WR)` 可形成 FIN 半关闭，随后仍可读取对端数据。
- 关闭和 fd 释放必须可重复、无泄漏。

## 4. 证据位置

```text
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-b-debug/
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-b-asan/
/home/win11/minidb-ah-final-20261106-v3-evidence/stage-b-ubsan/
```

每个目录包含 `summary.json`、server/client 日志和可用的 strace 记录。

## 5. 限制

- 这些实验用于理解 socket 行为，不是生产级 echo 服务。
- 数据和 fd 数量是特定 WSL2 运行结果，不应外推为吞吐或 SLA。
- 单个 run 不能替代 Reactor 层的完整生命周期测试；后者由阶段 C 的 214 项 CTest 覆盖。

## 6. 判定

**Stage B：PASS。**