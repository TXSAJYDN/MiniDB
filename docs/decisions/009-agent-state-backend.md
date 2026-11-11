# ADR-009：R4-D AI Agent 持久状态后端

- 状态：**Proposed** — 仅冻结设计；R4-D 实现尚未开始
- 日期：2026-11-12
- 关联：`docs/MiniDB-可靠性与持久化升级计划书.md`、ADR-002、ADR-003、ADR-004、ADR-006、ADR-007、ADR-008、`docs/wal-format.md`、`docs/recovery.md`、`docs/protocol-kv.md`
- 决策范围：R4-D 的持久布局、revision/CAS、幂等、session/tool 状态机、lease/fencing、二进制 KV 协议、资源上限、GC、checkpoint、恢复、失败语义、观测和验收门槛
- 准入条件：本 ADR 必须先以 docs-only 提交进入 `main`，并取得该提交自身的 9/9 GitHub Actions 通过；在此之前不得提交 R4-D 实现

## 背景

R4-A 到 R4-C 已经把 MiniDB 从单条写路径推进到组提交、后台 checkpoint 和基于
WAL v2 的快照隔离事务。当前内核已经具备本项目实现 Agent 状态后端所需的三个
关键边界：

1. 一次事务可以由一条 `kTxn` WAL 记录原子提交，并且恢复完整或完全不恢复；
2. 所有已确认写入按 LSN 顺序在 owner 线程 apply，读可以固定 snapshot LSN；
3. RDB、manifest、WAL replay 和版本 GC 继续以普通 committed KV 为权威状态。

这只解决了“能否原子保存多个 key”，还没有回答 Agent 场景更重要的问题：
同一个逻辑请求在网络重试、连接断开、进程崩溃和 lease 过期后，如何避免重复的
本地状态转换？多个 worker 如何在不引入分布式共识的前提下知道自己的写入是否
仍然有效？一个崩溃时处于 `RUNNING` 的外部工具，如何在重启后被安全接管？

R4-D 的目标不是把 MiniDB 变成通用向量库、消息队列或工作流引擎。它要提供一个
**单节点、可恢复、可幂等重放、带 fencing 的 AI Agent 持久状态后端**，使 Agent
运行循环中的 session、工具调用和临时记录不再依赖进程内 map 或外部数据库。

## 决策摘要

1. R4-D 建立在 R4-C 的 committed state、LSN、Snapshot Isolation 和 `kTxn`
   原子提交之上，不新增 WAL、RDB 或 manifest 格式版本。
2. 每个 Agent mutation 对应一条现有 `kTxn`；状态记录与幂等记录必须在同一个
   LSN 原子提交。
3. 所有 Agent 写操作都要求非空 `idempotency_key`；相同 key 和相同 canonical
   request 返回保存的原始 MDOR 响应，不重复写 WAL。
4. 所有可 CAS 记录使用严格 `revision:u64`。create 的 expected revision 必须为
   0；update 必须精确匹配已提交 revision。
5. 每个 session 有单调递增的 `last_fence:u64`。重新获取 lease 必须产生更大的
   fence；旧 fence 的 claim/complete/requeue 全部拒绝。
6. Agent 内部键全部位于保留命名空间；普通 KV 客户端不得读写该命名空间。
7. Agent 数据是普通 committed KV，自动进入现有 RDB checkpoint、manifest、WAL
   恢复和版本 GC，不建立第二套持久化格式。
8. 只保证本地状态和幂等结果原子提交；不保证外部工具副作用 exactly-once。
9. 单节点，不做分布式 lease、共识、Raft、跨节点路由、认证授权或多租户隔离。
10. 不修改 `LICENSE` 或 `COPYING`。

## 1. 定位与边界

### 1.1 目标

R4-D 为单个 MiniDB 进程内的 Agent 提供以下能力：

- session 的创建、读取、更新、关闭和清除；
- tool 的创建、读取、claim、complete 和 requeue；
- 基于 session lease 和 fence 的崩溃后安全接管；
- 全局 generic record 的 revision/CAS；
- 对客户端重试幂等的 write path；
- 进程崩溃、WAL 尾部半写、checkpoint 和重启后的可验证恢复。

### 1.2 非目标

- 不提供跨节点 session 亲和、分布式锁、lease 仲裁或一致性协议；
- 不提供用户认证、ACL、租户隔离、传输加密或静态加密；
- 不提供 session/tool/record 的游标遍历、全文检索或范围查询；
- 不提供外部工具调用的 exactly-once；只能保证本地“已记录结果”不重复提交；
- 不提供 HTTP Agent API；
- 不改变现有 HTTP `/kv` 的单请求隐式事务语义；
- 不把 Agent 状态暴露到普通 KV 键空间；
- 不承诺生产级 SLA、跨地域复制或无限保留。

### 1.3 与现有事务的关系

Agent 的每次 mutation 在持久化层是一个内部事务：

```text
validate request
  -> load committed records at the current owner snapshot
  -> build the complete new record set
  -> one kTxn containing state writes + idempotency record
  -> R4-C conflict admission
  -> one LSN / group commit
  -> owner apply completion
  -> publish Agent index and MDOR response
```

Agent 不允许绕过 `PersistenceManager` 直接写 `KvStore`，也不允许在客户端显式
`BEGIN` 事务中嵌套执行。协议层拒绝嵌套，而不是把 AGENT 变成事务内子命令。

## 2. 存储布局与兼容性

### 2.1 保留命名空间

内部 key 统一以以下 17 字节前缀开始：

```text
00 6d 69 6e 69 64 62 2d 61 67 65 6e 74 2d 76 31 00
```

对应的 C++ 字节串为：

```text
"\x00minidb-agent-v1\x00"
```

该前缀是**保留键空间**。普通 KV 和 HTTP 客户端对任何以该前缀开头的 key 执行
SET、GET、DEL、EXPIRE、TTL 或事务读写时，必须返回参数错误，不能因为 key 当前
不存在就允许后续写入。

### 2.2 内部 key 布局

```text
marker       = prefix + "m"
session      = prefix + "s\0" + session_id
tool         = prefix + "t\0" + session_id + "\0" + tool_id
idempotency  = prefix + "i\0" + idempotency_key
generic      = prefix + "r\0" + record_name
```

限制：

| 名称 | 字节上限 | 额外限制 |
|---|---:|---|
| session_id | 128 | 非空；不得包含 NUL |
| tool_id | 128 | 非空；不得包含 NUL |
| record_name | 128 | 非空；不得包含 NUL |
| tool_name | 256 | 非空；二进制安全 |
| idempotency_key | 128 | 1..128 字节；二进制安全 |

名称不做 Unicode 规范化，不做大小写折叠，不把两个字节串视为同一个名称。所有
长度按字节计算。上述布局的最大 key 仍小于现有 1024 字节 KV key 上限。

### 2.3 namespace marker

marker 的完整 value 为 8 字节：

```text
"MDAM" | version:u16=1 | reserved:u16=0
```

启动和首个 Agent mutation 的规则：

1. 如果命名空间为空：允许启动；第一次 Agent 写事务必须在同一个 `kTxn` 中
   创建 marker。
2. 如果 marker 存在且格式合法：允许启动，并从 committed state 重建索引。
3. 如果前缀下存在任何记录但 marker 不存在：拒绝启动，不能把用户旧数据误认作
   Agent 状态。
4. 如果 marker 损坏、版本未知、保留位非零或 key 与记录类型不匹配：拒绝启动。
5. marker 只能在 Agent 内部事务中创建或删除；普通 KV 路径永远不能修改它。

### 2.4 兼容性

- R4-D 不改变 WAL v2 的 outer record 或 MTXN payload 格式；
- R4-D 不改变 RDB v2、manifest 或 checkpoint 的格式版本；
- Agent 记录以普通 SET/DEL 进入 `kTxn`，因此旧版本 MiniDB 若忽略保留前缀，
  仍可能读出这些 key，但不能理解其语义；
- 升级前已经存在保留前缀 key 的数据库不会被静默迁移，必须在升级窗口前由用户
  清理或备份后重新导入。启动失败是安全默认值，不是可跳过的 warning；
- 降级不承诺支持包含 Agent 记录的数据库。

## 3. MDAR 记录封套

### 3.1 固定布局

所有 session、tool、idempotency 和 generic 记录都使用统一封套。整数全部为大端：

```text
magic[4]             = "MDAR"
version:u8           = 1
record_type:u8       = 1 session | 2 tool | 3 idempotency | 4 generic
status:u8            = record-type-specific state
reserved:u8          = 0
revision:u64
last_fence:u64
created_at_ms:i64
updated_at_ms:i64
lease_expires_at_ms:i64
payload_len:u32
payload[payload_len]
```

固定封套为 **52 字节**。所有字段必须规范编码；decoder 必须拒绝未知 version、
未知 type、非零 reserved、payload 越界、尾随字节和超过 64 KiB 的状态记录。

### 3.2 status 编码

session：

| code | 状态 |
|---:|---|
| 1 | ACTIVE |
| 2 | WAITING_TOOL |
| 3 | COMPLETED |
| 4 | FAILED |
| 5 | CANCELLED |

tool：

| code | 状态 |
|---:|---|
| 1 | PENDING |
| 2 | RUNNING |
| 3 | SUCCEEDED |
| 4 | FAILED |
| 5 | CANCELLED |

generic 和 idempotency 使用 status=0。任何 type/status 组合不合法时，恢复和读取
都必须 fail-stop，不能把未知状态降级为普通记录。

### 3.3 session payload

```text
"MSES" | version:u16=1 | reserved:u16=0
       | session_expires_at_ms:i64
       | state_len:u32
       | state[state_len]
```

`state` 是客户端提供的会话状态，二进制安全，长度由记录总上限约束。session
的 status、revision、last_fence、created/updated time 和 lease 从封套读取。
`session_expires_at_ms` 是绝对 Unix epoch 毫秒，运行时同时维护单调时钟 deadline。

### 3.4 tool payload

```text
"MTOL" | version:u16=1 | reserved:u16=0
       | tool_id_len:u16 | name_len:u16
       | request_len:u32 | result_len:u32
       | tool_id | name | request | result
```

`tool_id`、`name`、`request` 和 `result` 都是二进制安全字节串。tool 的
PENDING/RUNNING/终态由封套 status 表示，claim fence 由封套 last_fence 表示。
tool key 中已经带有 session/tool id，payload 中仍重复 tool id，用于恢复时交叉
校验 key 与记录内容。

**单记录上限冲突的处理规则：** tool request 和 tool result 各自可以接近 64 KiB，
但 MDAR 封套 + MTOL payload 的总长仍不得超过 64 KiB。因此二者不能同时达到
各自的字面上限；总记录上限是更强的约束。接受请求前必须按最终编码长度检查。

### 3.5 idempotency payload

```text
"MIDP" | version:u16=1 | reserved:u16=0
       | request_hash[32]          SHA-256 of canonical request
       | expires_at_ms:i64
       | canonical_request_len:u32
       | response_len:u32
       | response[response_len]    complete MDOR bytes
```

idempotency 记录不保存原始请求正文，只保存其长度和 SHA-256。canonical request
是子命令和其完整参数的确定性二进制序列，不做 Unicode 规范化、不排序参数、
不丢弃空参数。相同 key 的请求只有在 hash、长度和子命令语义都一致时才视为同一
请求；不同则返回 `IDEMPOTENCY_CONFLICT`。

R4-D 的写响应 payload 固定为空，因此 `response_len` 很小；读响应不进入
idempotency 缓存。这样 idempotency 记录可以稳定落在单记录上限内。

### 3.6 generic payload

generic record 的 payload 是 opaque bytes，长度上限由 64 KiB 总封套约束。R4-D
不解释其内容，也不对它做文本规范化。

## 4. revision 与 CAS

### 4.1 严格 revision

- 记录不存在时 revision 为 0；
- create 必须在 expected_revision=0 时成功，成功后 revision 从 1 开始；
- update 必须精确匹配当前 revision，成功后 revision 增加 1；
- read 不改变 revision；
- revision 达到 `UINT64_MAX` 时不得回绕，返回 `CAPACITY`；
- 一个 `kTxn` 内对同一 key 的多次写入在编码前折叠，折叠后的最终记录只能有
  一个 revision 增量。协议层不应生成重复 key 事务，遇到则拒绝。

### 4.2 错误区分

- expected_revision=0 且记录已存在：`ALREADY_EXISTS`；
- expected_revision>0 且记录不存在或 revision 不同：`VERSION_MISMATCH`；
- 记录存在但当前状态不允许该操作：`INVALID_STATE`；
- 记录 revision 已不可递增：`CAPACITY`。

客户端必须在每次 mutation 前读取目标记录，并在重试时使用最新 revision，除非它
正在重放同一个 idempotency key。

## 5. 幂等键

### 5.1 规则

- 所有 Agent mutation 都必须带 1..128 字节的 `idempotency_key`；
- key 在整个 MiniDB 实例内全局唯一，不按 session 作用域复用；
- TTL 范围为 1 分钟到 7 天，默认 24 小时；
- TTL 到期后记录可被 GC；到期后的同 key 请求被视为全新的 key；
- 相同 key + 相同 canonical request：直接返回已保存的完整 MDOR 字节，不分配
  LSN，不写 WAL，不改变任何 revision；
- 相同 key + 不同 canonical request：`IDEMPOTENCY_CONFLICT`，不执行新请求；
- 并发相同 key：两个内部事务都尝试写同一个 idempotency key，R4-C 的 pending
  key conflict 只允许一个提交。失败方可以返回 `CONFLICT`；客户端重试后会
  命中已提交记录并得到原始响应。

### 5.2 哪些结果写入幂等记录

只有已经产生持久状态变化的结果写入幂等记录：成功 create/update/close/purge、
成功 tool mutation、成功 lease mutation 和成功 record CAS。

确定性业务拒绝不写幂等记录，包括 `VERSION_MISMATCH`、`ALREADY_EXISTS`、
`LEASE_HELD`、`LEASE_EXPIRED`、`FENCED`、`INVALID_STATE`、
`RESOURCE_LIMIT`、`CAPACITY` 和 `IDEMPOTENCY_CONFLICT`。因此客户端修正
状态后可以用同一 key 重试；只有真正提交过状态转换的结果才被固定为原始 MDOR。

### 5.3 canonical request

canonical request 的生成规则：

```text
subcommand_ascii
| argc:u8
| repeat: arg_len:u32 BE | arg_bytes
```

参数保持客户端给出的顺序和字节内容；idempotency key 也包含在 canonical request
中。该字节串只用于 hash 和长度，不进入持久化正文。服务端必须从同一份已解析
参数生成 hash，不能在重新序列化时改变表示。

## 6. session 状态机

### 6.1 状态

```text
ACTIVE
WAITING_TOOL
COMPLETED
FAILED
CANCELLED
```

`COMPLETED`、`FAILED`、`CANCELLED` 是终态。终态不可恢复为 ACTIVE，不可再
执行 update、lease acquire/renew/release 或 tool create/claim/complete/requeue。
终态 session 只能被读取、purge 或被 GC 删除。

### 6.2 操作

| 子命令 | 语义 |
|---|---|
| SESSION_CREATE | 创建 session，初始状态 ACTIVE，revision=1 |
| SESSION_GET | 读取当前 session |
| SESSION_UPDATE | CAS 更新 state、目标 ACTIVE/WAITING_TOOL 和 session TTL |
| SESSION_CLOSE | CAS 关闭为 COMPLETED/FAILED/CANCELLED，并清除 lease |
| SESSION_PURGE | 仅删除终态且无 live lease 的 session 及其 tool |

`SESSION_UPDATE` 不能把状态改为终态；终态只能由 `SESSION_CLOSE` 产生。
`SESSION_CLOSE` 成功后，所有尚未进入终态的 tool 在语义上立即变为
`CANCELLED`，但为控制事务大小不要求逐条重写 tool 记录。`TOOL_GET` 对一个
父 session 已终态的 tool 返回有效状态 CANCELLED 的只读视图；tool mutation 则
返回 `INVALID_STATE`。purge 会用 DEL 原子删除这些 tool key，不携带大 value。

### 6.3 session TTL

- TTL 范围为 1 分钟到 30 天；
- 创建时计算并持久化绝对 `session_expires_at_ms`，运行时同时保留单调 deadline；
- `SESSION_UPDATE` 可以延长或缩短 TTL，但不能让已过期 session 复活；
- 过期 session 对外逻辑上不存在，`SESSION_GET` 和所有 mutation 返回
  `NOT_FOUND`；
- GC 可以把过期 session 视为虚拟 CANCELLED，并删除它及其 tool；
- lease 的到期时间不得超过 session 的到期时间；否则 acquire/renew 返回
  `INVALID_STATE`。

### 6.4 purge

purge 的准入条件：

1. session 是终态，或已经逻辑过期；
2. lease 不存在或已经过期；
3. expected revision 精确匹配；
4. 同一 `kTxn` 可以容纳 session、全部 tool 的删除项和 idempotency 记录。

purge 的 `kTxn` 只对 tool 发 DEL，不读取或重写 tool value。128 个 tool 的最坏
情况约为 130 个 key operation，低于 R4-C 的 256-key 上限；删除项不携带 value，
因此不会接近 1 MiB 的 MTXN payload 上限。

## 7. lease 与 fencing

### 7.1 lease 的位置

lease 不建立独立记录，保存在 session 封套中：

```text
last_fence:u64
lease_expires_at_ms:i64
```

`last_fence` 是 session 生命周期内单调递增的 fencing token。session 创建时为 0，
第一次成功 acquire 得到 1。没有 acquire 就没有 worker 有权执行 tool mutation。

### 7.2 规则

- lease TTL 范围为 1 秒到 1 小时；
- 未过期 lease 不允许被抢占，返回 `LEASE_HELD`；
- lease 到期后，新的 acquire 必须把 `last_fence` 严格增加 1；
- acquire、renew、release 都写 session revision；
- renew/release 必须同时匹配 expected revision 和 fence；
- lease 已过期时 renew/release 返回 `LEASE_EXPIRED`，不能复活旧 lease；
- 每次 acquire 成功后，旧 fence 永久失效；即使旧 worker 尚未意识到 lease
  已过期，也不能 claim/complete/requeue；
- fence 达到 `UINT64_MAX` 时返回 `CAPACITY`，不得回绕；
- 运行时比较使用单调时钟，持久化 deadline 是绝对 Unix 毫秒，重启后由重启时
  的 wall clock 和单调时钟共同换算。

### 7.3 子命令

| 子命令 | 关键参数 | 说明 |
|---|---|---|
| LEASE_ACQUIRE | id, ttl_ms, idem_key | 无 live lease 时获取，fence+1 |
| LEASE_RENEW | id, expected_revision, fence, ttl_ms, idem_key | 延长当前 live lease |
| LEASE_RELEASE | id, expected_revision, fence, idem_key | 主动释放，保留 last_fence |

lease acquire 不要求 expected revision，因为它的目的就是从当前 session 获得新
fence；但它仍然受 session 状态、TTL、资源上限和 idempotency 约束。renew/release
必须 CAS，防止旧 session revision 上的请求覆盖新状态。

## 8. tool 状态机

### 8.1 状态与转换

```text
PENDING --claim--> RUNNING
RUNNING --complete(SUCCEEDED|FAILED|CANCELLED)--> terminal
RUNNING --requeue--> PENDING
PENDING/RUNNING --parent session terminal--> effective CANCELLED
```

终态不可再改变。RUNNING 在崩溃后仍然保持 RUNNING，启动时不会自动把它改回
PENDING；新 worker 必须先获得新的 session fence，然后显式 requeue 或记录已知
完成结果。

### 8.2 操作

| 子命令 | 语义 |
|---|---|
| TOOL_CREATE | 在 ACTIVE/WAITING_TOOL session 中创建 PENDING tool |
| TOOL_GET | 读取 tool，包含父 session 终态的有效取消视图 |
| TOOL_CLAIM | PENDING -> RUNNING，要求 live lease 和匹配 fence |
| TOOL_COMPLETE | RUNNING -> SUCCEEDED/FAILED/CANCELLED，要求 live lease 和匹配 fence |
| TOOL_REQUEUE | RUNNING -> PENDING，要求 live lease 和匹配 fence |

所有 tool mutation 都在同一 `kTxn` 内更新父 session 的 revision 和 updated time，
即使 session 的业务 payload 不变。这样 tool mutation 与 session close、
lease 变更、purge 共享同一个写冲突边界，不会出现“父 session 已关闭但 tool
刚被 claim 成功”的已提交分叉。

### 8.3 崩溃恢复

- RUNNING tool 在重启后可见，客户端可读取其 request、fence 和 revision；
- 旧 fence 在 lease 到期并由新 worker acquire 后必然失效；
- 新 worker 可以 requeue 后重新 claim，或直接提交已知结果；
- 外部工具是否真的执行过无法由 MiniDB 判断；MiniDB 只保证本地结果转换和
  idempotency 记录不会重复提交；
- 不自动重放外部副作用，不做分布式 exactly-once 声明。

## 9. generic record

generic record 提供 Agent 的键值草稿、上下文引用和小型 JSON/二进制状态：

| 子命令 | 语义 |
|---|---|
| RECORD_GET | 读取 record |
| RECORD_CAS | expected_revision=0 创建，否则 CAS 更新 |

规则：

- record_name 1..128 字节，不包含 NUL；
- payload 是 opaque bytes，受 64 KiB 单记录上限；
- generic record 是全局作用域，不自动随 session purge 删除；
- R4-D 不提供 record 删除或遍历；删除/归档通过未来 ADR 评估；
- 达到 16,384 条或 64 MiB Agent logical bytes 时，新 create 返回 `CAPACITY`。

## 10. binary KV 协议

### 10.1 命令码

现有二进制 KV 增加：

| code | 命令 | 参数 |
|---:|---|---|
| 12 | AGENT | 第一个参数必须是 ASCII 子命令；其余参数按子命令解析 |

AGENT 不新增 HTTP 路由。HTTP 的 `/kv` 不接受 AGENT，也不能借 HTTP 的隐式
事务绕过保留命名空间。

### 10.2 允许的子命令

session/tool/lease：

```text
SESSION_CREATE
SESSION_GET
SESSION_UPDATE
SESSION_CLOSE
SESSION_PURGE
TOOL_CREATE
TOOL_GET
TOOL_CLAIM
TOOL_COMPLETE
TOOL_REQUEUE
LEASE_ACQUIRE
LEASE_RENEW
LEASE_RELEASE
```

generic：

```text
RECORD_GET
RECORD_CAS
```

未知子命令、参数数量错误、非规范整数或超过字段上限，返回协议错误并保持连接
可继续处理后续请求；如果客户端显式 BEGIN 了事务，AGENT 返回错误且不改变该
事务状态。

### 10.3 参数形式

所有整数使用 ASCII 十进制，不使用前导加号、空格或本地化格式；u64 必须能由
`from_chars` 完整消费。写命令最后一个或倒数第二个参数为 idempotency key，
可选最后参数为 idempotency TTL（毫秒）；省略时默认 24 小时。具体参数表如下：

```text
SESSION_CREATE   <session_id> <session_ttl_ms> <state> <idem_key> [idem_ttl_ms]
SESSION_GET      <session_id>
SESSION_UPDATE   <session_id> <expected_revision> <target_state> <session_ttl_ms> <state> <idem_key> [idem_ttl_ms]
SESSION_CLOSE    <session_id> <expected_revision> <final_state> <idem_key> [idem_ttl_ms]
SESSION_PURGE    <session_id> <expected_revision> <idem_key> [idem_ttl_ms]
TOOL_CREATE      <session_id> <tool_id> <tool_name> <request> <idem_key> [idem_ttl_ms]
TOOL_GET         <session_id> <tool_id>
TOOL_CLAIM       <session_id> <tool_id> <expected_tool_revision> <fence> <lease_ttl_ms> <idem_key> [idem_ttl_ms]
TOOL_COMPLETE    <session_id> <tool_id> <expected_tool_revision> <fence> <final_state> <result> <idem_key> [idem_ttl_ms]
TOOL_REQUEUE     <session_id> <tool_id> <expected_tool_revision> <fence> <idem_key> [idem_ttl_ms]
LEASE_ACQUIRE    <session_id> <lease_ttl_ms> <idem_key> [idem_ttl_ms]
LEASE_RENEW      <session_id> <expected_revision> <fence> <lease_ttl_ms> <idem_key> [idem_ttl_ms]
LEASE_RELEASE    <session_id> <expected_revision> <fence> <idem_key> [idem_ttl_ms]
RECORD_GET       <record_name>
RECORD_CAS       <record_name> <expected_revision> <value> <idem_key> [idem_ttl_ms]
```

`TOOL_CREATE` 的 tool revision 目标固定为 0；它同时 bump 父 session revision。
`TOOL_CLAIM`、`TOOL_COMPLETE`、`TOOL_REQUEUE` 的 expected revision 是
tool revision，父 session revision 由内部状态在提交时校验和推进。客户端如果
需要验证父 session 的 CAS，应在操作前读取 session，并按协议返回的最新 revision
重试。

### 10.4 MDOR 响应封套

合法 AGENT 应用结果的响应参数是一个二进制 MDOR 字节串；传输层使用现有
`VALUE` 响应：

```text
magic[4]          = "MDOR"
version:u8        = 1
status:u8
flags:u8          = 0
reserved:u8       = 0
revision:u64
fence:u64
lsn:u64
payload_len:u32
payload[payload_len]
```

固定部分 36 字节。响应状态：

| code | 名称 | 含义 |
|---:|---|---|
| 0 | OK | 更新成功 |
| 1 | CREATED | create 成功 |
| 2 | NOT_FOUND | 目标不存在或已逻辑过期 |
| 3 | VERSION_MISMATCH | expected revision 不匹配 |
| 4 | ALREADY_EXISTS | expected=0 但目标已存在 |
| 5 | LEASE_HELD | 未过期 lease 阻止抢占 |
| 6 | LEASE_EXPIRED | lease 已过期，旧操作不能复活 |
| 7 | FENCED | fence 小于当前 last_fence |
| 8 | INVALID_STATE | 当前 session/tool/record 状态不允许 |
| 9 | IDEMPOTENCY_CONFLICT | 同 key 不同 canonical request |
| 10 | RESOURCE_LIMIT | 单请求或字段超过上限 |
| 11 | CAPACITY | 全局条数或字节预算耗尽 |

写响应 payload 为空；GET 响应 payload 是规范 MDAR 记录或有效的只读视图。
`revision` 对写响应是目标记录的新 revision；对 GET 是读取记录 revision。
`fence` 是相关 session 的当前 fence；generic record 的 fence 为 0。
`lsn` 是成功 mutation 的提交 LSN；GET 和失败响应为 0。

应用层错误不关闭连接。只有协议 framing、参数长度、未知命令等传输/解析错误才
使用现有 `ERROR` 响应并遵循既有关闭策略。

### 10.5 断连和响应顺序

- 写请求一旦被 `PersistenceManager` 接收，断连不能取消已经提交的 WAL；
- completion 回调必须绑定 session 生命周期，析构后安全 no-op；
- 同一连接仍保持单请求在途，响应按 admission 顺序发送；
- 重连客户端用相同 idempotency key 重试，可以取得保存的原始 MDOR。

## 11. 安全、错误边界与命名空间碰撞

### 11.1 普通 KV 隔离

在 HTTP 和二进制 KV 的所有用户入口中，key 校验必须在读写之前执行：

```text
key starts with "\x00minidb-agent-v1\x00" -> invalid argument
```

包括 SET、GET、DEL、EXPIRE、TTL、事务私有写集和事务读。不能只在写路径拒绝，
因为读路径会把 Agent 状态泄露给普通客户端。

### 11.2 启动检查

启动顺序必须保证 Agent 校验发生在开始接受连接之前：

1. 完成 RDB/WAL 恢复；
2. 扫描 committed state 中保留前缀；
3. 校验 marker；
4. 严格解码每条 Agent 记录；
5. 重建 session/tool/idempotency/generic 索引；
6. 校验 tool 的父 session、tool 计数、revision 和资源上限；
7. 全部成功后开放监听；任何非法记录 fail-stop 并给出明确错误。

恢复器不得跳过未知 Agent 记录继续启动。跳过会把损坏或旧版本数据伪装成空状态。

### 11.3 错误优先级

同一请求可能同时满足多个错误条件时，使用固定优先级，保证测试和客户端重试可
预测：

1. framing、参数数量、字段长度和整数格式；
2. idempotency 命中/冲突；
3. 目标存在性和 session TTL；
4. expected revision / ALREADY_EXISTS；
5. 终态或非法状态；
6. fence 和 lease；
7. 单请求/全局资源上限；
8. 持久化提交冲突和 I/O 错误。

## 12. 资源上限与索引

### 12.1 硬上限

| 资源 | 上限 |
|---|---:|
| 物理 session 记录 | 1,024 |
| 物理 tool 记录 | 8,192 |
| 每个 session 的 tool | 128 |
| generic record | 16,384 |
| idempotency record | 8,192 |
| Agent logical bytes | 64 MiB |
| session state | 64 KiB（受总记录上限约束） |
| tool request | 64 KiB（受总记录上限约束） |
| tool result | 64 KiB（受总记录上限约束） |
| generic value | 64 KiB（受总记录上限约束） |
| session/tool/record name | 128 字节 |
| tool_name | 256 字节 |
| idempotency key | 128 字节 |
| GC 候选记录/批 | 64（一个 session+tool 组算一个候选组） |
| 单个 Agent kTxn | 256 key operations；实际目标 ≤131 |

### 12.2 logical bytes

`logical bytes` 定义为所有保留前缀下已提交 key 的字节数之和加上 value 的
字节数之和，不包括 C++ 容器和索引开销。该定义稳定、可在恢复时重算，便于拒绝
超限写入而不依赖 allocator 行为。marker、idempotency 记录和 tool 的大字段都
计入 64 MiB。

### 12.3 内存索引

索引只缓存可重建的元数据，不复制大 value：

- session 按 id 索引：revision、state、TTL、last_fence、lease、tool_count；
- tool 按 (session_id, tool_id) 索引：revision、state、fence、encoded size；
- idempotency 按 key 索引：expires_at、request hash、response size；
- generic 按 name 索引：revision、encoded size；
- 全局计数：物理条数、logical bytes、每 session tool 数。

索引更新必须发生在 `PersistenceManager` 的 owner-loop completion 之后。不能在
admission 时提前发布新 revision，否则读会看到尚未提交的状态。

## 13. GC、checkpoint 与恢复

### 13.1 checkpoint

Agent 记录没有独立 snapshot、manifest 或 WAL segment。它们和普通 KV 一起进入：

- RDB checkpoint 捕获；
- 后台 checkpoint 的 snapshot 复制；
- manifest 的 checkpoint LSN 和 live segment 集合；
- WAL v2 恢复；
- MVCC 版本链和版本 GC。

因此 Agent 状态不增加 R4-B/R4-C 的第二套发布协议。

### 13.2 GC

GC 只通过普通内部 `kTxn` 删除：

- 到期 idempotency record；
- 终态或逻辑过期、且 lease 已过期的 session；
- 上述 session 的全部 tool；
- 过期 generic 不自动删除（R4-D 无 generic TTL）。

规则：

- GC 在 owner loop 上触发，单批最多 64 个候选记录/组；
- 一个 session 组最多删除 1 个 session + 128 个 tool，仍作为 1 个候选组；
- 删除 session 组和其 tool 必须同一 `kTxn`，不能先删 session 留下孤儿；
- GC 不得绕过 `PersistenceManager` 直接改 `KvStore`；
- GC 失败如果是 I/O 或持久化 fail-stop，必须和普通写一样处理；如果是资源/冲突
  则记录 diagnostics 并在下一轮重试；
- GC 事务不生成幂等记录，因为它们没有客户端请求；
- 崩溃点前后都由 WAL 原子性决定：整个 GC 组存在或不存在。

### 13.3 恢复

恢复必须：

1. 读取 RDB 和 WAL 后在 committed state 上扫描保留前缀；
2. 验证 marker 和所有 MDAR；
3. 重建所有索引和计数；
4. 校验 tool 的父 session 必须存在；
5. 校验 tool 数不超过 128、全局资源不超限；
6. 不自动修改 RUNNING tool 或 lease；
7. 对已过期 lease/session 保持可读元数据，等待 GET 逻辑过期和 GC。

如果恢复后发现孤儿 tool、越界 payload、未知状态或 marker 缺失，启动失败。

### 13.4 与 R4-C 版本 GC 的关系

Agent 记录没有额外的“隐藏版本”或跨 LSN 引用。活跃快照只需要看到已提交的
普通版本；session/tool 的索引元数据从当前可见记录重建，不持有会阻止 R4-C
版本回收的私有快照。GC 只能删除已提交的最新记录，不得删除仍被活跃快照引用的
版本；具体版本回收仍由 R4-C 的版本链策略负责。

## 14. 失败矩阵

| 场景 | 允许结果 | 禁止结果 |
|---|---|---|
| 参数非法 | 返回应用/协议错误，无 LSN | 部分写入、产生 idempotency 记录 |
| expected revision 不匹配 | 返回 VERSION_MISMATCH | 覆盖新 revision |
| 未过期 lease 被抢占 | LEASE_HELD | fence 增加或状态改变 |
| 旧 fence claim | FENCED | 写入 tool RUNNING |
| lease 过期后 renew | LEASE_EXPIRED | 复活旧 lease |
| 同 key 不同请求 | IDEMPOTENCY_CONFLICT | 执行第二个请求 |
| 同 key 相同请求 | 返回原始 MDOR，无新 LSN | 重复写 WAL/重复 bump revision |
| 写队列满/字段超限 | RESOURCE_LIMIT/CAPACITY | 截断或静默丢弃 |
| WAL append/fsync 失败 | fail-stop，不 ack | 继续接受写或跳过 LSN |
| owner apply 前崩溃 | 重启后按 WAL 原子恢复 | 部分 session/tool 记录 |
| 客户端断连 | 已接收的提交继续完成；回调 no-op | 取消已持久化事务 |
| checkpoint 中崩溃 | 旧 manifest 或新 manifest 的既定边界 | 混合两个快照 |
| GC 中崩溃 | 整组删除存在或不存在 | 孤儿 tool 或半删 session |
| 恢复遇到坏 Agent 记录 | 启动失败并报告 | 跳过坏记录继续服务 |

## 15. 可观测性

R4-D 必须在现有 HTTP `/stats` 和 KV `STATS` 的 JSON 中增加稳定的
`agent` 对象；不新增需要认证的 HTTP Agent 接口。字段至少包括：

```text
sessions_stored
sessions_live
sessions_created
sessions_completed
sessions_failed
sessions_cancelled
sessions_purged
tools_stored
tools_pending
tools_running
tools_succeeded
tools_failed
tools_cancelled
records_stored
idempotency_stored
logical_bytes
logical_bytes_max
idempotency_replays
idempotency_conflicts
revision_mismatches
lease_acquires
lease_renews
lease_expiries
fenced_rejections
gc_runs
gc_candidates
gc_deleted
gc_failures
gc_total_us
gc_max_us
```

约定：

- 计数器和计数快照必须来自同一个 owner/persistence snapshot，不能把不同时刻的
  live 数与 max 拼接；
- `logical_bytes_max` 固定为 64 MiB；
- `tools_*` 统计有效状态（父 session 已终态的非终态 tool 计入 cancelled）；
- timing 只统计成功 GC，`total >= max`；
- 这些是诊断指标，不是 SLO，不提供任意分位数；
- 现有 group commit、transaction、checkpoint 字段保持兼容，不改变名字或类型。

## 16. 验收门槛

### 16.1 ADR 准入

- ADR-009 作为独立 docs-only 提交进入 `main`；
- 该提交触发 9 个 GitHub Actions 检查，全部 `completed/success`；
- 在 9/9 前不得提交实现代码。

2026-11-12 的准入记录：最初的 ADR docs-only 提交因旧工作流在 runner 上执行
`apt-get` 而无法完成，不能作为通过证据。工作流已单独修复为使用 Ubuntu 24.04
runner 预装的固定工具链，并取得 `500d67d` 自身的 9/9。包含本说明的 docs-only
提交是 R4-D 实现的实际准入提交；本提交自身必须再次取得 9/9，之后才允许提交
任何 R4-D 实现代码。该记录不改变 ADR 的存储布局或运行时语义。

### 16.2 实现测试

必须新增并通过：

- codec：所有 truncation、magic/version/type/status/reserved、长度溢出、尾随
  bytes、非规范 payload 和 64 KiB 边界；
- namespace：marker 创建、缺失 marker、碰撞、损坏 marker、启动 fail-stop；
- revision：create/CAS/update、UINT64 边界、并发同目标冲突；
- idempotency：replay 不新增 LSN、不同 canonical request 冲突、TTL、崩溃后
  replay、并发同 key 只提交一次；
- session：非法状态转换、终态不可变、TTL、purge 前置条件和 tool 原子删除；
- lease：过期、fence 严格递增、旧 fence 拒绝、renew/release CAS 和时钟跳变；
- tool：claim/complete/requeue、RUNNING 崩溃后仍可见、新 fence 接管、
  父 session 终态有效取消；
- generic：创建、CAS、边界和容量；
- protocol：AGENT code 12、子命令参数、MDOR、显式事务拒绝、断连安全；
- recovery：RDB/WAL/checkpoint 混合恢复、坏记录 fail-stop、无孤儿 tool；
- resource：所有条数、字段和 64 MiB 上限的精确边界；
- process integration：至少覆盖 idempotency replay、lease takeover、RUNNING
  tool 崩溃恢复和 namespace 碰撞；
- fuzz：新增 MDAR/MDOR codec 和 AGENT 协议 fuzz target，现有四个 target 不退化；
- observability：HTTP 和 KV stats 的 Agent 字段契约。

### 16.3 质量门禁

实现完成后必须重新执行，不得继承旧提交结果：

- Debug；
- Release；
- ASan；
- UBSan；
- ASan + UBSan；
- TSan，使用 `setarch x86_64 -R`；
- 全部 CTest；
- 进程集成测试；
- 全部 fuzz target 的固定种子 smoke；
- clang-format `--dry-run --Werror`；
- clang-tidy；
- `git diff --check`；
- `python3 -m py_compile tests/process_integration_test.py`；
- 最终实现提交自身的 9/9 GitHub Actions。

历史测试数量必须按新配置实际采集；不得把 R4-A 的 338/24、R4-B 的 348/25 或
R4-C 的既有数字机械复制为 R4-D 证据。

## 17. 实施顺序

1. **AgentRecordCodec**：key 编码、MDAR/MDOR 封套、严格解码、边界和 fuzz。
2. **AgentStateStore**：namespace marker、session/tool/idempotency/record 索引、
   revision/CAS、fencing、资源计数和恢复重建。
3. **PersistenceManager 集成**：内部 `kTxn` 构造、状态 + idempotency 同 LSN、
   completion/fail-stop 和 owner-loop 边界。
4. **AGENT 协议**：KvProtocol code 12、MDOR 参数/错误映射、显式事务拒绝和断连。
5. **Server 启动与观测**：namespace 碰撞检查、Agent stats、GC timer、
   shutdown/checkpoint 接入。
6. **测试与文档**：上述验收矩阵、进程测试、fuzz、protocol/architecture/
   recovery/troubleshooting 文档和最终证据。

每个实现步骤必须保持工作树可构建、可测试和可回滚；不得把无关格式升级、许可证
修改或 HTTP Agent API 混入 R4-D 提交。

## 18. 未决事项与后续边界

以下事项明确不在 R4-D 首批实现内，必须另立 ADR，不得在本 ADR 中偷偷扩大范围：

- 多节点 lease、Raft/共识和跨节点恢复；
- 认证、授权、多租户隔离和传输/静态加密；
- 外部工具 exactly-once；
- generic record 删除、TTL 和遍历；
- session/tool 分页列表和跨 session 查询；
- Agent 事件订阅、Webhook、流式 token 存储；
- 自定义 checkpoint 周期或独立 Agent snapshot；
- license/COPYING 变更。

ADR 后续变更必须记录原因、兼容性影响和是否需要新格式版本。若实现发现必须在
这里冻结的语义之外扩展，应先修改 ADR 并重新取得 docs-only 提交的 9/9 CI，而
不是先写代码再补文档。

## 19. 收益与限制

### 收益

- 将 Agent 的 session、tool 和幂等状态纳入已有事务/持久化内核，而不是维护
  第二套难以恢复的内存状态；
- 一次 mutation 的本地状态与幂等结果同 LSN 原子可见，重试可返回原始响应；
- lease/fencing 为崩溃后的 worker 接管提供明确的单调 token；
- 所有资源有硬上限，单个 Agent 事务仍然有界；
- 复用现有 RDB checkpoint、WAL replay、group commit 和版本 GC。

### 限制

- 单节点，不解决节点故障转移和跨节点会话迁移；
- 不保证外部工具副作用 exactly-once；
- 终态 session 的子 tool 可能保留到 purge/GC，查询需要理解“有效取消”语义；
- generic record 无删除/遍历，不能替代完整文档数据库；
- lease 依赖客户端正确传播 fence；忽略 fence 的外部执行器仍可能产生重复副作用；
- 64 MiB 是逻辑 key+value 预算，不包含 C++ allocator 和索引 overhead；
- 不提供生产 SLA、审计合规或认证授权。

## 20. 结论

R4-D 选择“普通 committed KV 上的版本化 Agent 记录 + 单 kTxn 幂等提交 + session
lease/fence”作为实现边界。它不新增持久化格式，不改变 R4-A/R4-B/R4-C 的已确认
语义，并用严格 revision、幂等和启动 fail-stop 把可恢复性变成可测试契约。

本 ADR 当前状态为 **Proposed**。只有在 docs-only ADR 提交取得自身 9/9 CI 后，
才允许开始实现；实现完成和证据采集通过后，才可把状态更新为 Accepted。
