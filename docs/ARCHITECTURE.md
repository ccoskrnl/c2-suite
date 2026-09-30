# C2 远控系统架构设计计划书

- **版本**：v0.1（设计稿）
- **日期**：2026-08
- **技术栈**：客户端 C++（Windows，功能模块化 dll）｜服务端 Go（C2 中转/管理服务器）｜控制端 Go CLI
- **传输**：TCP + 自定义二进制协议 + AES-256-GCM 加密
- **设计目标**：基于现有 RAT 演示 demo 的设计重写为**可扩展**的 C2 架构——文件管理、系统管理、远程控制等功能以**独立 dll 插件**形式加载，统一通讯协议（心跳、数据包结构、指令码），客户端与服务端可独立演进。

---

## 目录

1. [总体架构与拓扑](#1-总体架构与拓扑)
2. [统一通讯协议规范](#2-统一通讯协议规范)
3. [插件框架与 C++ 客户端设计](#3-插件框架与-c-客户端设计)
4. [Go 服务端与 Go CLI 设计](#4-go-服务端与-go-cli-设计)
5. [目录结构、构建方案与开发里程碑](#5-目录结构构建方案与开发里程碑)

---

## 1. 总体架构与拓扑

### 1.1 部署拓扑

系统采用 **C2（Command & Control）中转模型**：所有被控端（Agent）主动外连 Go C2 服务器，控制端（Go CLI）也连接同一台 C2 服务器；C2 统一维护会话、转发指令并回传结果。

```
┌───────────────────────────┐        ┌──────────────────────────────┐        ┌────────────────────┐
│     C++ Agent (Windows)   │  TCP   │      Go C2 服务器            │  TCP   │    Go CLI 控制端    │
│                           │  AES   │  (中转 / 会话管理 / 指令路由) │  AES   │                    │
│  RatAgent.exe (core)      │◄──────►│  端口 4444（Agent + CLI 共用）│◄──────►│  ratctl            │
│  ├── plugin_remote.dll    │  GCM   │  ├─ 会话注册表（多 Agent）    │  GCM   │  list / exec /      │
│  ├── plugin_file.dll      │        │  ├─ 指令路由（双向）          │        │  upload / download │
│  └── plugin_sys.dll       │        │  └─ CLI 通道                 │        │  screenshot / ...  │
└───────────────────────────┘        └──────────────────────────────┘        └────────────────────┘
```

设计要点：

- **Agent 主动外连**：解决 NAT / 防火墙导致的控制端无法主动连接被控端的问题（与现有 demo 的
  Agent 主动 `connect_host` 一致）。
- **C2 统一管理**：多 Agent 会话集中注册在 C2，控制端无需知道 Agent 的网络位置，只与 C2 通信。
- **插件化**：Agent 的业务功能（远程控制 / 文件管理 / 系统管理）各自独立为 dll，由 core 按需
  加载；新增功能 = 新增插件 dll + 新增指令码段，不改动核心通信逻辑。
- **单端口复用**：Agent 与 CLI 都连 C2 的 4444 端口，通过**握手包中的角色字段**（`ROLE_AGENT` /
  `ROLE_CONTROL`）区分，减少运维面。

### 1.2 核心数据流

两类连接、三条数据流：

```
① 心跳/保活      Agent ──CMD_HEARTBEAT──▶ C2            （周期 15s）
② 指令下发       CLI ──业务指令──▶ C2 ──转发──▶ Agent
③ 结果/事件回传  Agent ──CMD_RESULT──▶ C2 ──回传──▶ CLI
```

- **上行（Agent → C2）**：心跳、指令执行结果（`CMD_RESULT`，携带请求 seq 关联）、插件主动
  事件上报（`CMD_EVENT`，如传输进度、截图完成）。
- **下行（C2 → Agent）**：控制端（经 C2 转发的）业务指令。
- **控制通道（CLI ⇄ C2）**：管理类指令（如 `list` 会话列表）由 C2 本地处理并直接回复；
  业务类指令（如 `exec`）由 C2 查会话表后**透明转发**给目标 Agent。

### 1.3 会话与指令生命周期

1. **连接**：Agent/CLI 建立 TCP 连接 → 发送 `CMD_HELLO`（含角色、协议版本、Agent 携带的
   插件清单）→ 双方派生会话密钥 → 会话注册。
2. **保活**：Agent 每 15s 发 `CMD_HEARTBEAT`（携带 agent_id、插件清单、负载指标）；
   C2 30s 未收到心跳 → 标记离线；60s → 回收会话。
3. **指令**：CLI 发 `(seq, cmd, payload)` → C2 按目标 agent_id 查会话 → 转发 →
   Agent core 按指令码路由到对应插件执行 → 插件回包 `(同 seq, status, response)` →
   Agent 发 `CMD_RESULT` 回 C2 → C2 按请求的 CLI 会话回传。
4. **断线重连**：Agent 指数退避重连（1s→2s→4s→…→上限 60s），重连后重新握手注册。

### 1.4 分层架构与模块职责边界

两侧均采用分层设计，各层职责单一、以接口解耦，便于单层替换与测试。

**C++ Agent（被控端）分层**

| 层 | 模块 | 职责 | 依赖 |
|---|---|---|---|
| 传输层 | `core/net` | TCP 连接、断线重连、读写完整性（`send_all`/`recv_all`） | Winsock |
| 协议层 | `core/protocol` | 数据包编解码（分帧/组包）、AES-GCM 加解密、checksum | 传输层 |
| 会话层 | `core/session` | 握手、心跳、会话状态机、seq 请求/响应关联 | 协议层 |
| 插件层 | `core/plugin_loader` + 各 `plugin_*.dll` | 插件发现/加载/卸载、指令码→插件路由、`IPlugin` 接口 | 协议层 |
| 业务层 | 三个插件 dll 内部 | 远程控制 / 文件管理 / 系统管理的具体实现 | 插件层接口 |

约束：**core 不包含任何业务逻辑**（不写文件、不抓屏、不注入），只做通信、路由与生命周期；
所有业务都在插件 dll 内。新功能只新增插件，不触碰 core。

**Go C2 服务器（服务端）分层**

| 层 | 模块 | 职责 |
|---|---|---|
| 网络层 | `internal/network` | 监听/accept、连接对象封装、读写超时 |
| 协议层 | `internal/protocol` | 与 C++ 侧同一规范的包编解码、AES-GCM 解密/加密 |
| 会话层 | `internal/session` | 会话注册表（agent 会话 + control 会话）、心跳超时判定、回收 |
| 路由层 | `internal/router` | 指令转发（control→agent）、结果回传（agent→control）、管理指令本地处理 |
| 应用层 | `cmd/c2server` | 配置加载、启动/关闭、日志 |

**Go CLI（控制端）分层**

| 层 | 模块 | 职责 |
|---|---|---|
| 连接层 | `ctl/internal/conn` | 连接 C2、加密会话 |
| 命令层 | `ctl/cmd/ratctl` | 命令解析（参数/标志）、调用的指令码构造 |
| 输出层 | `ctl/internal/out` | 表格 / JSON 输出格式化 |

边界约定（两端统一）：

- **协议规范是唯一契约**：两端各自独立实现 `protocol` 层，只以 `ARCHITECTURE.md` 第 2 章的
  字节布局与指令码表为准，不跨层共享代码。
- **指令码决定路由**：指令码高 4 位为模块段（见 2.2），C2 与 Agent 的路由都按此分发，
  新增模块不修改路由框架本身。

---

## 2. 统一通讯协议规范

协议是**唯一契约**：C++ 客户端与 Go 服务端各自独立实现本规范，字节布局以本章为准。

### 2.1 数据包结构

所有消息（Agent↔C2、CLI↔C2）使用同一数据包，网络字节序统一为 **little-endian**。

```
偏移    字段        类型      说明
0       magic       u16       固定 0x5A5A，快速识别/丢弃噪音
2       version     u8        协议版本（当前 1）
3       flags       u8        bit0=payload 已加密(1)；bit1=payload 已压缩(预留)
4       seq         u32       序列号：请求/响应关联（CLI 发起时递增，C2 原样带回）
8       timestamp   u32       发送方本机时间戳（GetTickCount / time.Now ms）
12      cmd         u16       指令码（见 2.2 指令码表）
14      plugin      u8        插件 ID（0=core；其余对应插件 dll）
15      status      u8        状态码（0=OK；非 0 为错误码，响应用）
16      payload_len u32       payload 字节数（0 ≤ len ≤ 16 MiB）
20      payload     bytes     AES-GCM 密文（含 12B nonce 前缀 + 16B tag）
---     end
```

接收端分帧规则（流式，无粘包问题）：

1. 读固定 20 字节头部 → 校验 `magic`/`version`；
2. 按 `payload_len` 读取剩余字节；
3. 若 `flags.bit0=1`：`payload = nonce(12) + ciphertext + tag(16)`，用会话密钥
   AES-256-GCM 解密得到明文；GCM tag 兼作完整性校验（失败即丢包并计数）。
4. 头部明文传输（便于 C2 在不解密时也能路由到目标会话；敏感信息一律放 payload）。

**错误码（status）** 统一：`0`=OK，`1`=未实现/未知指令，`2`=参数错误，`3`=权限不足，
`4`=目标不存在（文件/进程等），`5`=超时，`0x80+`=插件自定义错误。

### 2.2 指令码表（分模块）

指令码按模块分段，**高 4 位 = 模块段**：`0x0` 核心、`0x1` 远程控制、`0x2` 文件管理、
`0x3` 系统管理、`0x4` 管理（仅 CLI↔C2）。低 12 位为模块内序号。

**核心段（plugin=0，core 处理）**

| 指令码 | 名称 | 方向 | payload（明文） | 响应 payload |
|---|---|---|---|---|
| 0x0101 | CMD_HELLO | Agent/CLI→C2 | 版本+角色+agent_id+插件清单 | C2 会话信息+server 时间 |
| 0x0102 | CMD_HELLO_ACK | C2→Agent/CLI | 会话密钥派生参数/接受标志 | — |
| 0x0103 | CMD_HEARTBEAT | Agent→C2 | agent_id+插件清单版本+负载 | —（C2 不单独回，靠收包时间） |
| 0x0104 | CMD_RESULT | Agent→C2→CLI | 请求 seq+status+结果 | —（回传用） |
| 0x0105 | CMD_EVENT | Agent→C2→CLI | 插件事件（类型+数据） | —（异步通知） |
| 0x0106 | CMD_BYE | 任意→C2 | 原因码 | — |

**远程控制段（plugin=1，plugin_remote.dll）**

| 指令码 | 名称 | 方向 | payload | 响应 |
|---|---|---|---|---|
| 0x1001 | RMT_SCREENSHOT | CLI→Agent | 空（或目标分辨率/质量） | PNG/JPEG 数据 |
| 0x1002 | RMT_SCREEN_STREAM | CLI→Agent | 开始/停止+帧率+区域 | 连续 CMD_EVENT 帧 |
| 0x1003 | RMT_MOUSE_MOVE | CLI→Agent | i32 x, i32 y（绝对坐标） | status |
| 0x1004 | RMT_MOUSE_CLICK | CLI→Agent | 按键(左/右/中)+按下/释放 | status |
| 0x1005 | RMT_MOUSE_WHEEL | CLI→Agent | i16 delta | status |
| 0x1006 | RMT_KEY | CLI→Agent | vk(u8)+按下/释放+扩展标志 | status |
| 0x1007 | RMT_SET_CAPTURE_REGION | CLI→Agent | i32 x/y/w/h（0 表示全屏） | status |

**文件管理段（plugin=2，plugin_file.dll）**

| 指令码 | 名称 | 方向 | payload | 响应 |
|---|---|---|---|---|
| 0x2001 | FILE_LIST | CLI→Agent | 路径(utf-8) | 条目数组（名称/大小/类型/时间） |
| 0x2002 | FILE_DOWNLOAD | CLI→Agent | 远程路径 | 分块数据（块号+数据+是否末块） |
| 0x2003 | FILE_UPLOAD | CLI→Agent | 块号+目标路径+数据+是否末块 | 进度/status |
| 0x2004 | FILE_DELETE | CLI→Agent | 路径 | status |
| 0x2005 | FILE_RENAME | CLI→Agent | 旧路径+新路径 | status |
| 0x2006 | FILE_EXECUTE | CLI→Agent | 路径+参数 | status+pid |

**系统管理段（plugin=3，plugin_sys.dll）**

| 指令码 | 名称 | 方向 | payload | 响应 |
|---|---|---|---|---|
| 0x3001 | SYS_INFO | CLI→Agent | 空 | 系统信息（OS/CPU/内存/磁盘） |
| 0x3002 | SYS_PROC_LIST | CLI→Agent | 空 | 进程数组（pid/名称/内存） |
| 0x3003 | SYS_PROC_KILL | CLI→Agent | pid | status |
| 0x3004 | SYS_SVC_LIST | CLI→Agent | 空 | 服务数组（名称/状态/启动类型） |
| 0x3005 | SYS_SVC_CTRL | CLI→Agent | 服务名+操作(启动/停止/重启) | status |
| 0x3006 | SYS_REG_READ | CLI→Agent | 键路径+值名 | 类型+值 |
| 0x3007 | SYS_REG_WRITE | CLI→Agent | 键路径+值名+类型+值 | status |

**管理段（plugin=0xFF，仅 CLI↔C2，由 C2 本地处理不转发）**

| 指令码 | 名称 | 方向 | payload | 响应 |
|---|---|---|---|---|
| 0x4001 | CTL_LIST | CLI→C2 | 空 | 在线会话表（agent_id/ip/平台/插件清单/心跳时间） |
| 0x4002 | CTL_KILL_SESSION | CLI→C2 | agent_id | status（通知 Agent 断开） |
| 0x4003 | CTL_GET_SESSION | CLI→C2 | agent_id | 单会话详情 |

> 约定：`0x10xx/0x20xx/0x30xx` 业务指令由 CLI 发出，C2 只按 payload 中或头部指定的
> 目标 agent 透明转发，**不解密、不解析业务载荷**；结果经 `CMD_RESULT` 原路回传。

### 2.3 心跳、会话生命周期与断线重连

**心跳包**

- Agent 每 **15s** 发送一次 `CMD_HEARTBEAT`（0x0103），payload：`agent_id(16B) + 插件清单
  版本(u32) + 插件位图(u32，bit i 表示 plugin_id=i 已加载) + 负载指标(CPU%u8、内存%u8)`。
- 心跳**不需要 C2 单独应答**：C2 以收到任意包（含心跳）的时间刷新该会话的最后活跃时间；
  网络保活同时依赖 TCP keepalive（30s 探测）。

**会话状态机**

```
CONNECTING → HELLO_SENT → REGISTERED ⇄ OFFLINE（心跳超时）→ CLOSED
```

| 事件 | 判定 | 动作 |
|---|---|---|
| 收到 CMD_HELLO | 版本/角色校验通过 | 注册会话（agent_id 冲突则踢掉旧会话） |
| 30s 未收到任何包 | 标记离线 | `REGISTERED → OFFLINE`，通知 CLI（可选） |
| 离线超过 60s | 回收 | 关闭连接、删除会话记录 |
| 收到 CMD_BYE / 连接断开 | 主动/被动下线 | 清理会话 |

**断线重连（Agent 侧）**

- 连接断开后指数退避重连：`1s → 2s → 4s → 8s → …`，上限 60s，无限重试；
- 重连成功后重新走 HELLO 握手（**不恢复旧会话**，seq 重新从 0 开始）；
- 重连期间正在执行的指令（若有）超时失败，由 CLI 侧按 seq 超时判定。

### 2.4 加密方案（AES-256-GCM）

**目标**：防窃听、防篡改（GCM 认证加密）、无证书依赖、可平滑升级为 TLS。

- **密钥来源**：预共享密钥 PSK（部署时写入两端配置文件，Agent 侧可编译期内置），
  32 字节；**仅用于握手阶段派生会话密钥**，不直接加密业务包。
- **握手派生**（在 `CMD_HELLO`/`CMD_HELLO_ACK` 内完成）：
  1. 客户端生成 16 字节随机数 `client_nonce`，随 HELLO 发送（明文段内）；
  2. C2 生成 16 字节 `server_nonce`，随 HELLO_ACK 返回；
  3. 双方计算 `session_key = HKDF-SHA256(PSK, client_nonce || server_nonce, "rat-v1")`
     （32 字节）；此后所有包的 payload 用 `session_key` 加密。
- **每包加密**：`nonce(12B 随机) + AES-256-GCM(session_key, nonce, plaintext)` →
  密文段 = `nonce(12) + ciphertext + tag(16)`；`flags.bit0=1` 标记已加密。
- **头部明文**：magic/version/flags/seq/cmd/plugin/status/payload_len 不加密
  （C2 不解密即可路由）；敏感数据一律在 payload 内。
- **防重放（预留）**：seq + 时间戳由接收方维护滑动窗口，M3 起启用。
- **升级路径**：协议层只依赖"会话密钥"，未来将 HKDF 派生替换为 TLS-PSK 或证书交换即可，
  指令码与数据包结构不变。

---

## 3. 插件框架与 C++ 客户端设计

### 3.1 插件框架（IPlugin 接口与加载机制）

插件以 **dll** 形式分发，core 动态加载。公共接口定义在 `plugin_api.h`（core 与各插件共享）。

**插件描述与接口（草案）**

```cpp
// plugin_api.h（core/include/）
#pragma once
#include <cstdint>
#include <vector>

// 插件描述：core 用它在注册表里做路由
struct PluginInfo {
    uint32_t api_version;   // 本接口版本，当前 1；core 校验，不匹配拒绝加载
    uint8_t  plugin_id;     // 插件 ID：1=remote, 2=file, 3=sys（与协议 plugin 字段一致）
    const char* name;       // 插件名，如 "plugin_remote"
    const char* version;    // 插件版本，如 "1.0.0"
    uint16_t cmd_lo;        // 本插件处理的指令码段 [lo, hi)
    uint16_t cmd_hi;
};

// 插件运行上下文：core 注入的回调，插件通过它发送异步结果/事件
struct PluginContext {
    // 发送响应（status + payload），关联到请求 seq
    void (*send_result)(uint32_t req_seq, uint8_t status,
                        const uint8_t* payload, uint32_t len);
    // 主动上报事件（如传输进度、屏幕帧）
    void (*send_event)(const uint8_t* payload, uint32_t len);
};

class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual const PluginInfo& info() const = 0;
    // 初始化：core 在 LoadLibrary 后调用；返回 false 则卸载
    virtual bool init(const PluginContext* ctx) = 0;
    virtual void shutdown() = 0;
    // 处理一条属于本插件的指令；req_seq 用于异步场景回包
    virtual bool handle_command(uint16_t cmd, uint32_t req_seq,
                                const uint8_t* payload, uint32_t len) = 0;
};

// dll 导出约定（extern "C" 工厂）
extern "C" {
    __declspec(dllexport) IPlugin* plugin_create();
    __declspec(dllexport) void     plugin_destroy(IPlugin*);
}
```

**core 插件加载器（`core/plugin_loader`）职责**

1. 启动时扫描 `plugins/` 目录下的 `plugin_*.dll`；
2. `LoadLibraryW` → 取 `plugin_create` → 校验 `api_version` 与 `plugin_id` 唯一性 →
   注入 `PluginContext` 回调 → `init`；失败则 `FreeLibrary` 并记日志；
3. 维护路由表 `plugin_id → IPlugin*` 与 `[cmd_lo, cmd_hi) → IPlugin*`；
4. 收到业务包（cmd 高 4 位 ≠ 0x0/0x4）时，按 plugin 字段或指令码段查表分发；
5. 退出时逆序 `shutdown` + `plugin_destroy` + `FreeLibrary`。

**路由规则**

- 指令码 `0x0xxx` → core 自己处理（握手/心跳/结果）；
- `0x1xxx`→plugin 1、`0x2xxx`→plugin 2、`0x3xxx`→plugin 3（按高 4 位，与 `plugin` 字段
  双重校验，不一致视为协议错误）；
- `0x4xxx` 只出现在 CLI↔C2，Agent 收到即丢弃并告警。

**插件开发约束**（写在接口注释与文档中）

- 插件内不得直接调用 core 私有实现，只能使用 `PluginContext` 回调和 `plugin_api.h`；
- 耗时的指令（文件传输、抓屏流）必须在插件自己的线程执行，用 `send_result`/`send_event`
  异步回包，禁止阻塞 core 的接收线程；
- 插件崩溃不得拖垮 core：指令在插件线程执行，core 通过心跳/结果超时兜底。

### 3.2 core 模块与三个插件功能清单

**core（`RatAgent.exe` 或 `core.dll`，不依赖业务）**

| 模块 | 职责 |
|---|---|
| `net` | TCP 连接/重连（指数退避）、`send_all`/`recv_all`、连接状态回调 |
| `protocol` | 20B 头编解码、分帧、AES-GCM 加解密（基于 2.4 会话密钥） |
| `session` | HELLO 握手、心跳线程（15s）、会话状态机、seq 关联表 |
| `plugin_loader` | 插件扫描/加载/路由（3.1） |
| `config` | 读取配置（C2 地址/端口、PSK、重连参数），支持命令行覆盖 |
| `main` | 组装各模块、优雅退出 |

**plugin_remote.dll（plugin_id=1，指令码 0x1xxx）——屏幕监控 + 键鼠注入**

- `RMT_SCREENSHOT`：GDI 抓一帧 → PNG/JPEG 编码 → `send_result`；
- `RMT_SCREEN_STREAM`：抓屏线程按帧率/区域持续抓帧，经 `send_event` 发帧流
  （复用现有 demo 的 `ScreenSpy` + 抓屏线程，改造为插件内部线程 + 事件上报）；
- `RMT_MOUSE_MOVE/CLICK/WHEEL`、`RMT_KEY`：`SendInput` 注入
  （复用现有 `input_replay.cpp`，去掉直连 socket，改为 core 回调）；
- `RMT_SET_CAPTURE_REGION`：动态调整抓取区域（复用现有区域参数设计）。

**plugin_file.dll（plugin_id=2，指令码 0x2xxx）——文件管理**

- 列目录（名称/大小/类型/修改时间）；
- 下载：按 64KiB 分块读取并回传（块号+末块标志），支持进度事件；
- 上传：按块写入，服务端/CLI 分块发送，支持覆盖校验；
- 删除/重命名；执行本地程序（`CreateProcess`，返回 pid）。
- 边界：路径统一 UTF-8 编解码；禁止路径穿越（校验绝对路径与盘符合法性）。

**plugin_sys.dll（plugin_id=3，指令码 0x3xxx）——系统管理**

- 系统信息：OS 版本、CPU/内存/磁盘容量与占用；
- 进程：枚举（`CreateToolhelp32Snapshot`）/ 结束（`TerminateProcess`）；
- 服务：枚举（`EnumServicesStatusEx`）/ 启动/停止/重启（`OpenService` + 控制）；
- 注册表：读/写（`RegGetValue`/`RegSetValueEx`，支持 DWORD/QWORD/字符串/二进制）。

### 3.3 对现有 demo 的代码复用清单

| 现有文件 | 复用去向 | 改造点 |
|---|---|---|
| `src/agent/screen_spy.cpp/.h` | `plugin_remote` | 保留核心；增加区域参数动态设置、帧回调 |
| `src/agent/capture_thread.cpp` | `plugin_remote` | 发送改走 `send_event`，生命周期归插件 |
| `src/agent/input_replay.cpp` | `plugin_remote` | 去掉直连 socket 依赖 |
| `src/agent/desktop_switch.cpp` | `plugin_remote` | 原样保留 |
| `src/common/net.cpp` | `core/net` | 增加重连与连接状态回调 |
| `src/common/protocol.h` | `core/protocol` | 按第 2 章规范重写（新包头/指令码/加密） |
| `src/viewer/input_hook.cpp` | **不直接复用** | CLI 端为 Go，无 LL 钩子；遥控改由 CLI 显式下发坐标/按键 |
| `e2e_*.py` 测试思路 | `server/test` | 保留端到端驱动思路，按新协议重写 |

---

## 4. Go 服务端与 Go CLI 设计

### 4.1 Go 服务端（c2server）模块设计

```
cmd/c2server/main.go        入口：配置加载、启动各模块、信号处理
internal/network/           网络层
  conn.go                   TCP 连接封装：读写、超时、关闭
  server.go                 监听/accept、连接分发（按 HELLO 角色进 agent/control 池）
internal/protocol/          协议层（与 C++ 同规范，见第 2 章）
  packet.go                 20B 头编解码、分帧
  crypto.go                 AES-256-GCM、HKDF-SHA256 会话密钥派生
internal/session/           会话层
  registry.go               会话注册表（map[agent_id]*Session / map[connID]*ControlSession）
  heartbeat.go              心跳扫描（30s 离线 / 60s 回收）
  session.go                Session 结构（agent_id/IP/平台/插件位图/最后活跃/连接）
internal/router/            路由层
  router.go                 指令分发：管理指令本地处理；业务指令转发到目标 agent
  relay.go                  结果/事件回传：Agent→C2→对应 CLI（按 seq 找发起方）
internal/ctl/               管理指令实现（0x4xxx）
  list.go / kill.go / get.go
internal/config/            配置：监听地址、PSK、心跳超时参数
```

**关键设计**

- **角色分流**：accept 后读 HELLO，`ROLE_AGENT` 进 agent 会话池，`ROLE_CONTROL` 进 CLI 池；
  两类连接都可多路（多 CLI 登录、多 Agent）。
- **透明转发**：业务指令（0x1xxx/0x2xxx/0x3xxx）C2 不解密、不解析，仅按 payload 中的
  `agent_id` 找到会话转发；结果按 seq 关联回传发起请求的 CLI。
  - 指令里如何带目标 agent_id？约定：**业务指令 payload 首 16 字节固定为 `agent_id`**，
    C2 据此路由，转发给 Agent 时保留原 payload（Agent 侧插件忽略或校验该字段）。
- **任务/结果关联**：`router` 维护 `(req_seq, cli_conn)` 映射，Agent 的 `CMD_RESULT` 带
  `seq`，C2 查表回传；无匹配则丢弃并告警。
- **并发模型**：每连接一个读 goroutine；写操作按连接加锁；会话注册表用 `sync.RWMutex`。
- **可观测性**：结构化日志（slog）、会话事件（上线/离线）可选写入 SQLite（M4 里程碑）。

### 4.2 Go CLI（ratctl）命令集

连接：`ratctl -s <c2_addr> [-k <psk>] <command>`；首次连接自动握手。

| 命令 | 说明 | 示例 |
|---|---|---|
| `list` | 在线会话列表 | `ratctl list` |
| `session <id>` | 查看单会话详情 | `ratctl session abc123` |
| `exec <id> -- <cmd>` | 远程执行程序 | `ratctl exec abc123 -- notepad.exe` |
| `ls <id> <path>` | 列目录 | `ratctl ls abc123 C:\Users` |
| `upload <id> <local> <remote>` | 上传文件 | `ratctl upload abc123 a.exe C:\x.exe` |
| `download <id> <remote> <local>` | 下载文件 | `ratctl download abc123 C:\x.exe ./x.exe` |
| `rm <id> <path>` | 删除文件 | `ratctl rm abc123 C:\x.exe` |
| `screenshot <id> <out.png>` | 截屏保存 | `ratctl screenshot abc123 shot.png` |
| `remote --move/--click/--key <id> ...` | 键鼠注入 | `ratctl remote --click abc123 --x 100 --y 200` |
| `sysinfo <id>` | 系统信息 | `ratctl sysinfo abc123` |
| `ps <id>` / `kill <id> <pid>` | 进程管理 | `ratctl ps abc123` |
| `svc <id>` / `svc <id> start <name>` | 服务管理 | `ratctl svc abc123` |
| `reg <id> read <path> <name>` | 注册表读 | `ratctl reg abc123 read HKLM\... Value` |
| `kill-session <id>` | 踢下线 | `ratctl kill-session abc123` |

输出规范：默认人类可读（表格）；`-j` 输出 JSON（便于脚本化）；所有命令非零退出码表示失败。

**CLI 内部结构**：`cmd` 包做参数解析 → 构造指令包（seq 自增、加密）→ 连接 C2 →
发指令 → 阻塞等 `CMD_RESULT`（带 30s 超时）→ 格式化输出。

---

## 5. 目录结构、构建方案与开发里程碑

### 5.1 新项目目录结构（建议 `rat/c2/`）

```
rat/c2/
├── ARCHITECTURE.md              # 本计划书（复制为设计基线）
├── docs/
│   ├── protocol.md              # 第 2 章协议规范独立成文（两端开发者共同引用）
│   └── plugin-dev.md            # 插件开发指南（IPlugin 接口 + 示例）
├── client/                      # C++ Agent（Windows）
│   ├── CMakeLists.txt           # 顶层（core + 插件统一构建，也可用 .sln）
│   ├── core/
│   │   ├── include/             # plugin_api.h、protocol.h 等公共头
│   │   └── src/
│   │       ├── net/ protocol/ session/ plugin_loader/ config/ main.cpp
│   ├── plugins/
│   │   ├── remote/              # plugin_remote.dll
│   │   ├── file/                # plugin_file.dll
│   │   └── sys/                 # plugin_sys.dll
│   └── tools/                   # 打包脚本（dll 与 exe 一并分发）
├── server/                      # Go C2
│   ├── go.mod
│   ├── cmd/c2server/
│   └── internal/{network,protocol,session,router,ctl,config}
└── ctl/                         # Go CLI
    ├── go.mod                   # 与 server 同仓库多模块，或合并为单模块
    ├── cmd/ratctl/
    └── internal/{cmd,conn,out}
```

### 5.2 构建方案

- **C++ 客户端**：推荐 CMake（插件各自独立 target，输出到 `plugins/`；core 为可执行）。
  若沿用现有习惯也可用 VS 解决方案；**插件必须与 core 使用同一 ABI 头 `plugin_api.h`**。
  构建产物：`RatAgent.exe` + `plugin_remote.dll` + `plugin_file.dll` + `plugin_sys.dll`。
- **Go**：`server/` 与 `ctl/` 各一个 `go.mod`（或单模块多 cmd），`go build ./...`；
  依赖仅标准库（crypto/aes、crypto/cipher、crypto/hmac、golang.org/x/crypto/hkdf）。
- **联调测试**：`server/test/` 放 Go 端到端测试（起 c2server → 模拟 Agent 握手/心跳 →
  CLI 指令全链路）；C++ 侧保留现有 `e2e_*.py` 思路做协议级驱动测试。

### 5.3 开发里程碑

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| **M1 协议与核心通信** | 两端实现第 2 章协议：20B 头、AES-GCM、握手/心跳；Agent core 连接 C2；c2server 会话注册 | 真实 Agent 上线，`list` 可见；心跳 30s 离线判定生效 |
| **M2 插件框架 + 远程控制** | `plugin_api.h`、加载器；plugin_remote 截图/注入（复用 demo） | CLI 下发 screenshot/click 在目标机生效 |
| **M3 文件与系统插件** | plugin_file（分块传输）、plugin_sys（进程/服务/注册表） | upload/download 大文件完整；ps/reg 命令返回正确 |
| **M4 管理完善与收尾** | 重连、防重放、多 CLI、会话历史 SQLite（可选）、打包脚本、文档 | 断网重连恢复会话；完整 README + 协议文档 |

每个里程碑结束提交可运行版本并跑通对应验收项。

### 5.4 与现有 demo 的关系

- 本计划书描述的是**新项目 `rat/c2/`**，与 `rat/desktop`（现有 demo）并存；
- 现有 demo 继续作为**参考实现**：`screen_spy`/`input_replay`/`net` 等按 3.3 复用清单移植；
- 协议与 demo 不兼容（新协议 + 加密），两端同步重写。

---

*本文档为架构设计计划书（v0.1）。实施时以第 2 章协议规范为唯一契约；协议字段或指令码
变更须同步更新 `docs/protocol.md` 并在两端回归。*
