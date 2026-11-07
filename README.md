# MiniDB

MiniDB 是一个使用 C++20 实现的紧凑型、单节点、教学用途键值数据库。它通过两个相互独立的前端访问同一份内存存储：

- 用于检查和简单自动化的严格 HTTP/1.1 子集；
- 用于 API 级 CRUD、TTL 和显式快照的二进制 TCP 协议。

网络栈、HTTP/二进制解析器、事件循环、TTL 存储和 RDB 格式均在本仓库内实现，没有委托给数据库或 Web 框架。本项目面向学习、确定性测试和系统级基准测试，**不是生产数据库**。

> 许可证：由项目所有者暂缓决定（DEFERRED BY OWNER）。

## 打开与快速开始

### 1. 打开项目

推荐使用 Visual Studio Code 或 CLion 打开仓库根目录。

- Windows 资源管理器：打开 `E:\c++\miniDB`
- PowerShell：执行 `code E:\c++\miniDB`
- WSL/Ubuntu 终端：执行以下命令

~~~bash
cd /mnt/e/c++/miniDB
code .
~~~

> MiniDB 依赖 Linux 的 epoll、eventfd、timerfd 和 signalfd，不能在 Windows 上使用 MSVC 原生构建和运行。请在 WSL2 或 Linux 环境中编译、测试和运行。
>
> `/mnt/e` 适合日常开发；正式性能基准和验收建议在 WSL 的 Linux 原生文件系统（例如 `$HOME`）中执行。

### 2. 安装依赖

Ubuntu 24.04 可执行：

~~~bash
sudo apt update
sudo apt install -y build-essential cmake git python3 curl
~~~

首次运行 CMake 配置时需要联网下载 GoogleTest v1.14.0；也可以提前提供本地 GoogleTest 源码目录。

### 3. 构建 Release 版本

~~~bash
cmake --preset release
cmake --build --preset release -j2
ctest --preset release --output-on-failure
~~~

构建产物位于：

~~~text
build/release/app/minidb_http_server
build/release/app/minidb_kv_server
~~~

### 4. 启动并使用 HTTP 服务

在第一个 WSL 终端中启动服务器：

~~~bash
./build/release/app/minidb_http_server -p 8080 -r /tmp/minidb-http.rdb
~~~

在第二个 WSL 终端中执行读写操作：

~~~bash
curl -i http://127.0.0.1:8080/health
curl -i -X POST --data-binary 'hello' http://127.0.0.1:8080/kv/greeting
curl -i http://127.0.0.1:8080/kv/greeting
curl -i -X DELETE http://127.0.0.1:8080/kv/greeting
curl -i -X POST http://127.0.0.1:8080/save
~~~

也可以在浏览器中打开 `http://localhost:8080/health` 做健康检查。MiniDB 没有网页管理界面，根路径不会显示控制台；增删改查和保存操作应使用 HTTP API 或二进制 KV 客户端。

按 `Ctrl+C` 停止服务器。收到 `SIGINT` 或 `SIGTERM` 时，服务器会执行最终快照保存，然后退出。

### 5. 启动并使用二进制 KV 服务

~~~bash
./build/release/app/minidb_kv_server -p 6379 -r /tmp/minidb-kv.rdb
~~~

在另一个终端中运行冒烟客户端：

~~~bash
python3 test_kv_client.py 6379
~~~

HTTP 和 KV 服务不能同时使用同一个 RDB 路径。项目没有实现多进程文件锁。

### 6. 验证重启持久化

上面的服务会分别把数据保存到 `/tmp/minidb-http.rdb` 和 `/tmp/minidb-kv.rdb`。停止服务后，用相同命令和相同 `-r` 路径重新启动，数据会从 RDB 快照恢复。

## 已实现的 P0 能力

- Linux **水平触发（level-triggered）epoll reactor**，每个服务器进程只有一个事件循环属主线程。
- 非阻塞 `accept`、增量读取、短写处理、背压、每连接有界缓冲区以及优雅关闭。
- 带严格消息分帧和显式大小限制的 HTTP/1.1 解析器。
- 二进制安全的 KV 协议，每条连接同一时刻只允许一个在途命令。
- 支持惰性过期和周期性过期扫描的内存键值存储。
- RDB v2 快照，包含 CRC32C、有界内存分配、临时文件写入、文件和目录 `fsync` 以及原子重命名。
- 先恢复到临时存储，再原子发布；损坏或截断的快照会被拒绝，不会替换正在使用的存储。
- HTTP 和 KV 的命令行契约测试、重启持久化测试、损坏文件启动测试以及关闭时保存失败测试。

## 明确的限制

- 不提供 TLS、身份认证、授权、复制、集群、分片或事务。
- 不支持 HTTP/2、分块请求体（chunked）、`Expect: 100-continue` 或 WebSocket 升级。
- 所有客户端 I/O 都由一个事件循环线程处理；TTL 清理器是唯一的后台线程。
- 每个服务器进程最多支持 32 个并发连接。
- RDB v2 是唯一支持的快照版本，不提供 v1 迁移路径。
- 同一个 RDB 路径只能由一个服务器进程使用，项目没有实现跨进程文件锁。
- 主要在 WSL2 下的 Ubuntu 24.04 上验证。其他 Linux 环境需要重新验证，尤其是 TSan 和性能结论。

## 环境要求

- Linux；权威开发和验收环境为 WSL2 下的 Ubuntu 24.04。
- CMake 3.14 或更新版本；使用仓库中的 CMake presets 时建议 CMake 3.21 或更新版本。
- GCC 13 或更新版本，或者较新的 Clang。
- Python 3，用于进程测试、基准测试和 fuzz-smoke 工具。
- 首次执行 CMake 配置时需要网络访问，以便 FetchContent 获取 GoogleTest v1.14.0；也可以提供本地 GoogleTest 源码目录。

## 构建与测试

使用 CMake presets：

~~~bash
cmake --preset debug
cmake --build --preset debug -j2
ctest --preset debug --output-on-failure
~~~

也可以显式配置：

~~~bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure
~~~

仓库提供 Debug、Release、ASan、UBSan、ASan+UBSan 和 TSan 六种配置预设。

在当前 WSL2 内核上，TSan 测试需要通过 `setarch x86_64 -R` 启动，以规避高熵 ASLR 导致的启动失败：

~~~bash
setarch x86_64 -R ctest --preset tsan --output-on-failure
~~~

如需运行完整的新鲜六配置矩阵并保留日志：

~~~bash
python3 tools/run_acceptance_matrix.py \
  --source "$PWD" \
  --root /home/win11/minidb-ah-final-20261106-v3-matrix \
  --evidence /home/win11/minidb-ah-final-20261106-v3-evidence \
  --gtest-source "$PWD/build/_deps/googletest-src"
~~~

构建目录和证据目录必须不存在或为空；脚本不会删除或复用已有目录。最终验收矩阵在每种配置下都运行了 214 个 CTest 用例。

## 运行

HTTP 服务默认监听 8080 端口，RDB 默认为当前目录下的 `minidb.rdb`：

~~~bash
build/release/app/minidb_http_server -p 8080 -r /tmp/minidb-http.rdb
~~~

KV 服务默认监听 6379 端口，RDB 默认为当前目录下的 `minidb.rdb`：

~~~bash
build/release/app/minidb_kv_server -p 6379 -r /tmp/minidb-kv.rdb
~~~

两个程序共用相同的命令行参数：

~~~text
-p <port>      监听端口
-r <path>      RDB 文件路径
-h             显示帮助；必须单独使用
~~~

HTTP 冒烟检查：

~~~bash
curl -i http://127.0.0.1:8080/health
curl -i -X POST --data-binary 'hello' http://127.0.0.1:8080/kv/greeting
curl -i http://127.0.0.1:8080/kv/greeting
~~~

针对 KV 服务运行二进制客户端冒烟脚本：

~~~bash
python3 test_kv_client.py 6379
~~~

`SIGINT` 和 `SIGTERM` 会触发优雅保存并结束进程。只有最终快照写入以及父目录持久化步骤都成功时，退出状态才为 0。

## 验收状态

完整的阶段 A-H 技术验收已在 `docs/A_H_ACCEPTANCE_REPORT.md` 中记录为 **PASS**。

验收代码基线提交为 `9da4b48f5fe78e4c2896ad1ca07464361b27e362`。该提交触发的 GitHub Actions 运行为
[42182440543](https://github.com/TXSAJYDN/MiniDB/actions/runs/42182440543)，9 个任务全部成功：5 项构建/测试矩阵、TSan、格式检查、静态分析和固定随机种子的 fuzz smoke。

验收机器上的证据根目录为 `/home/win11/minidb-ah-final-20261106-v3-evidence`，其中包含六配置矩阵、Stage B socket 运行记录、Stage G 64 MiB RDB/恢复测量、Stage H fuzz/静态分析/基准测试、CI 页面截图以及一次公开贡献尝试记录。

许可证决定暂由项目所有者保留。仓库未创建 `LICENSE` 或 `COPYING` 文件，不得将本项目表述为已经采用经过批准的开源许可证。

项目曾对 [eclipse-4diac/4diac-forte#1135](https://github.com/eclipse-4diac/4diac-forte/issues/1135) 进行一次真实公开贡献尝试：创建并测试了提交 `ac80baaf7df26ff1f3ae50d28b19acece4db9ccf`，但上游推送和 fork 推送均未获授权，匿名创建 PR 返回 HTTP 401，因此没有创建拉取请求。

## 文档

- [系统架构](docs/architecture.md)
- [HTTP 协议](docs/protocol-http.md)
- [KV 二进制协议](docs/protocol-kv.md)
- [RDB v2 格式](docs/rdb-format.md)
- [部署与运维](docs/deployment.md)
- [故障排查](docs/troubleshooting.md)
- [基准测试方法](bench/README.md)
- [开发者工具](tools/README.md)
- [Fuzz 测试](fuzz/README.md)
- [依赖说明](docs/dependencies.md)
- [项目状态](docs/PROJECT_STATE.md)

## 目录结构

~~~text
app/           HTTP 和 KV 服务器入口
src/           网络、协议、存储等核心实现
tests/         单元测试、集成测试和契约测试
bench/         基准测试工具与方法
fuzz/          libFuzzer 模糊测试目标
experiments/   实验性目标
tools/         验收、格式化和开发辅助脚本
docs/          架构、协议、部署和验收文档
~~~

## 仓库状态

实现、测试、CI 工作流和最终验收报告均由 `main` 分支跟踪并已推送。本地构建产物不应提交到仓库。

本项目仍然是教学用途的单节点数据库。通过 A-H 验收并不意味着已经达到生产可用标准，文档中列出的功能限制和环境约束仍然适用。
