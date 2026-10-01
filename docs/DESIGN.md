# C2 + Agent + Ctl 系统设计文档

> 版本：随代码同步（2026-08）｜对应提交：`149afe5` 及之前
> 本文档描述**当前实现**的设计实况（区别于 `ARCHITECTURE.md` 的原始规划）。

---

## 1. 系统总览

三端、三条信道：

```
┌─ RatAgent.exe (C++) ────────────────────────────────┐
│  默认入口：长连接模式（TCP 4444）                     │
│  ├ net / protocol / crypto / session                │
│  ├ cmd_dispatch（命令分发中心：SYS_INFO/EXEC_PS/...） │
│  ├ screen_spy（GDI 抓屏）                            │
│  ├ screen_stream（屏幕流长连接线程 + 输入注入）        │
│  ├ input_replay / desktop_switch（SendInput 注入）    │
│  └ beacon（HTTP 轮询备选入口，可切换）                 │
└──────────┬──────────────────────┬──────────────────┘
           │ TCP 4444 (ctl 信道)   │ TCP 4445 (屏幕流信道)
           ▼                      ▼
┌─ c2server.exe (Go) ────────────────────────────────┐
│  main.go      TCP 4444：握手/会话/relay/转发         │
│  beacon.go    HTTP 8080：beacon 轮询（GET命令/POST结果）│
│  beacon_stream.go TCP 4445：屏幕流握手/配对/双向中继  │
│  session.go   会话注册表（超时/回收）                 │
└───────┬──────────────────────────┬─────────────────┘
        │ TCP 4444                 │ TCP 4444
        ▼                          ▼
┌─ ratctl.exe (Go) ──┐   ┌─ im_ctl.exe (C++ ImGui) ─┐   ┌─ viewer.exe (C++ Win32) ─┐
│ 交互式 shell        │   │ GUI：会话/命令/屏幕显示    │   │ 由 ratctl screen 命令拉起  │
│ sessions/use/sysinfo│   │ + 键鼠 hook（输入回放）    │   │ 建流 + 显示 + 键鼠注入     │
│ /exec/screen/payload│   │ + Docking                │   │ 无 ImGui/OpenGL 依赖      │
└─────────────────────┘   └──────────────────────────┘   └──────────────────────────┘
```

| 端 | 语言 | 默认信道 | 角色 |
|---|---|---|---|
| RatAgent | C++ (Windows) | TCP 4444 长连接（`run_agent`）；beacon HTTP 8080 备选（`run_beacon`） | 被控端：抓屏/执行命令/注入输入/落盘执行 payload |
| c2server | Go | 监听 4444/8080/4445 | C2 中枢：会话/中继/转发 |
| ratctl | Go | TCP 4444 | 控制端（CLI）：会话/系统/截屏/拉起 viewer/payload |
| im_ctl | C++ (ImGui) | TCP 4444 | 控制端（GUI） |
| viewer | C++ (Win32) | TCP 4444 | 控制端（屏幕流查看器，由 ratctl 拉起） |

---

## 2. 通信协议

统一数据包（`c2/internal/protocol`、`client/core/protocol.h`）：

```
[4B payload_len LE][1B type][payload]
类型包（TCP 信道）头部 20B：magic(0x5A5A)+ver(1)+flags+seq+timestamp+cmd+plugin+status+payload_len
```

### 2.1 加密与密钥（三把钥匙，互相独立）

| 密钥 | 派生 | 用途 |
|---|---|---|
| **会话密钥** | `HKDF-SHA256(PSK, client_nonce‖server_nonce, "rat-v1")` | ctl 信道（Agent↔C2 握手后、ctl↔C2）所有 payload |
| **beacon_key** | `HKDF-SHA256(PSK, agent_id, "rat-beacon-v1")` | beacon 信道（无状态，服务器按 agent_id 重算） |
| **stream_key** | `HKDF-SHA256(PSK, agent_id‖client_nonce‖server_nonce, "rat-stream-v1")` | 屏幕流信道（4445） |

- 加密：AES-256-GCM（12B 随机 nonce + 密文 + 16B tag），`flags.bit0` 标记加密；头部明文（C2 不解密可路由）
- PSK：三端编译期内置 32B（`0102...20`），生产需外部化

### 2.2 指令码表（当前已实现）

| 段 | 指令码 | 名称 | 说明 |
|---|---|---|---|
| 核心 | 0x0101/02 | CmdHello / CmdHelloAck | 握手（明文） |
| | 0x0103 | CmdHeartbeat | 心跳（TCP agent） |
| | 0x0104 | CmdResult | 结果回传（req_seq+status+result） |
| | 0x0106 | CmdBye | 下线 |
| 业务 | 0x3001 | SysInfo | 系统信息（OS/CPU/内存/磁盘/主机名） |
| | 0x3008 | ExecPs | PowerShell 执行 |
| | 0x3009 | Payload | payload 分块上传 → `%TEMP%\c2_payload\` → CreateProcess 执行（协议 §10） |
| | 0x1001 | Screenshot | 单帧截屏（响应为 JPEG 原始字节） |
| | 0x1002 | ScreenStream | 屏幕流控制（beacon/长连接下发：control+fps+quality+scale） |
| 屏幕流 | 0x0501/02 | StreamHello/Ack | 4445 握手（明文） |
| | 0x0503 | ScreenFrame | 帧包（seq+w+h+jpeg_len+JPEG，stream_key 加密） |
| | 0x0504 | InputEvents | 输入事件（agent_id+count+InputEvent[24B]） |
| 管理 | 0x4001 | CtlList | 会话列表（ctl→C2 本地） |
| | 0x4002 | CtlKillSession | 踢会话下线（ctl→C2 本地） |
| | 0x4003 | CtlGetSession | 单会话详情（ctl→C2 本地，payload = agent_id 的 hex 字符串） |

### 2.3 防重放（协议 §4.1）

- 每连接维护 `lastSeq`，请求类包要求 `seq` 严格递增（TCP 有序前提下安全）
- **数据/回显包不参与**（避免 seq 空间冲突）：
  - ctl 读线程：`SCREEN_FRAME` 先于防重放处理（帧 seq 是 Agent 侧递增，独立空间）
  - 服务器 agentLoop：`CMD_RESULT` 跳过防重放（命令 seq 回显，与心跳 seq 分属两个计数器）
- **控制端响应投递顺序**（`im_ctl/ctl_client.cpp`，viewer 共用）：
  - 发送侧：`seq` 在发送锁**内**分配，保证 seq 递增顺序 = 字节落网顺序（否则 C2 防重放会丢包）
  - 接收侧：**先按 seq 查等待者并投递**，只有"无人认领"的包才走 `lastSeq` 闸门
    （否则并发的"晚到的较早响应"会被闸门永久吞掉）
- 历史教训：早期帧/CMD_RESULT 与请求 seq 共用窗口，导致"start 后所有命令 busy"（已修复 `149afe5`）

---

## 3. 三条信道详解

### 3.1 ctl 信道（TCP 4444）

- 握手：HELLO（明文，role=AGENT/CONTROL + agent_id + client_nonce）→ HELLO_ACK（server_nonce）→ 派生会话密钥
- 角色分流：`role=AGENT` → agentLoop（心跳/CMD_RESULT 回传）；`role=CONTROL` → ctlLoop（管理指令 + 业务指令处理）
- 业务指令流转：
  - **长连接 Agent**（Beacon=false）：服务器**直接转发**给 Agent 连接（`target.WritePacket`），seq 用 `target.NextSeq()`
  - **beacon Agent**（Beacon=true）：入 BeaconQueue，Agent 下次 GET 取走
  - 均登记 relay `(agent_id, seq) → (cli, cliKey, cliSeq)`，结果经 `CMD_RESULT` 原路回传
- 管理指令（CTL_LIST 等）：C2 本地处理直接回复

### 3.2 beacon 信道（HTTP 8080）

- 轮询模型（协议 §8）：Agent 每 `sleeptime ± 20% jitter` 周期：先 POST 结果 → 再 GET 命令
- GET `/`（Cookie `SID=agent_id`）：注册/刷新会话，返回伪装 HTML，命令藏在 `<div id="token">base64(加密命令块)</div>`
- POST `/submit`（form `data=base64(加密结果块)`）：结果按 `(agent_id, seq)` 关联 relay 回传 ctl
- 命令/结果 TLV：`count + {seq, cmd, len, payload}` / `{seq, status, len, result}`
- 会话维护：beacon 会话离线/回收按 3x/6x sleeptime（默认 60s → 180s/360s）

### 3.3 屏幕流信道（TCP 4445）

- 触发：ctl 发 `ScreenStream(0x1002, control=1)` → Agent `cmd_dispatch` 启动 `screen_stream` 线程 → 主动外连 4445
- 握手：StreamHello（agent_id+client_nonce）→ StreamAck（server_nonce）→ 派生 stream_key
- 配对：`StreamRegistry` 维护 `agentID → (agent 屏幕连接, 配对 ctl 连接)`；ctl 发 start 时登记等待，Agent 连上后配对（`paired=true`）
- **帧上行**：Agent 抓屏（ScreenSpy，2K@32bpp）→ WIC JPEG 编码（~15-30ms/帧，~400KB）→ stream_key 加密 → 4445 → 服务器**转换加密**（stream_key → ctl 会话密钥）转发给配对 ctl
- **输入下行**：ctl LL hook 采集键鼠 → `InputEvents(0x0504)`（agent_id+count+events）→ 服务器去前缀、转 stream_key 加密 → 转发 Agent → `SendInput` 注入
- 停止：`control=0` → Agent `stop_screen_stream`（等线程退出 ≤5s）→ 断开 4445 → 服务器 `RemoveAgent`
- 竞态防护（`149afe5` 前修复）：`RemoveAgent` 只删自己的连接；`RegisterAgent` 踢旧先删表再关连接

---

## 4. 各端实现

### 4.1 c2server（Go，`server/cmd/c2server`）

| 模块 | 职责 |
|---|---|
| `main.go` | 监听 4444；握手（doHandshake/parseHello）；agentLoop/ctlLoop；`RelayMap`（业务结果回传）；业务指令按 agent 模式分发（beacon 入队 / 长连接直发） |
| `beacon.go` | HTTP 8080：GET 命令下发（伪装 HTML + token）、POST 结果接收、`BeaconQueue` 命令队列、beacon 会话注册 |
| `beacon_stream.go` | TCP 4445：屏幕流握手（StreamKey）、`StreamRegistry` 配对、帧上行转换加密转发、输入下行 |
| `internal/session` | 会话注册表（byID/byKey，RWMutex）、SweepLoop（TCP 30s 离线/60s 回收；beacon 180s/360s） |

### 4.2 RatAgent（C++，`client/core`）

| 模块 | 职责 |
|---|---|
| `main.cpp` | 默认入口 `run_agent`（TCP 4444 长连接：握手/心跳/读包分发）；`run_beacon`（HTTP 轮询备选）；`agent.id` 持久化 |
| `session` | 握手、心跳线程（15s，seq 递增） |
| `cmd_dispatch` | 命令分发中心：SysInfo（RtlGetVersion/GlobalMemoryStatusEx/GetDiskFreeSpaceEx）、ExecPs（CreateProcess powershell + 管道捕获 + 120s 超时）、ScreenStream 控制、Screenshot（GDI 抓帧 + WIC JPEG）、Payload（分块落盘 `%TEMP%\c2_payload\` + CreateProcess 执行） |
| `screen_spy` | GDI 抓屏（虚拟屏幕/DPI/区域/全分辨率） |
| `screen_stream` | 4445 连接线程：握手 → 帧循环（抓屏+WIC JPEG+加密上行，节奏由 `fps` 时间片控制、质量由 `quality` 经 WIC 属性包设置）+ 输入接收（自持重装缓冲 `InputStream` + `SendInput` 注入）；start/stop 线程状态机（`g_thread_alive` + `ThreadGuard`，可自愈重启） |
| `input_replay`/`desktop_switch` | 注入实现（WM_* 映射、坐标换算、活动桌面切换） |
| `beacon` | HTTP 轮询（GET 命令/POST 结果/TLV/jitter） |

### 4.3 控制端

**ratctl（Go，`ctl/cmd/ratctl/`）** —— 交互式 shell：
- 单条 ctl 长连接 + 后台读线程（解密/防重放/按 seq 分发）
- `sendAndWait`（240s 超时）、指令 seq 全局递增
- 会话：`sessions / use(前缀匹配) / back / session <id> 详情 / kill <id>`
- 业务：`sysinfo / exec / screenshot [id] <file> / payload <file> [name] [--args <参数>]`
- 远控：`screen [id] [fps] [quality] [scale]`（拉起 viewer）、`stop`（停流兜底）
- 提示符 `ratctl[agent]>`

**viewer（C++ Win32，`viewer/`）** —— 轻量远控查看器：
- 复用 `im_ctl/ctl_client.cpp`（CONTROL 客户端：连接/握手/业务/输入事件）与 `client/core` 协议栈
- 无 ImGui/OpenGL 依赖：Win32 窗口 + `StretchDIBits` 显示，WIC 解码 JPEG
- **由 viewer 自己发起 0x1002**：C2 按"发起流请求的那条 ctl 连接"配对转发帧，
  因此 ratctl 的 `screen` 只负责拉起进程并透传 `-s/-a/-fps/-quality/-scale`
- 键鼠：WH_MOUSE_LL/WH_KEYBOARD_LL 采集 → 图像矩形内坐标换算 → 0x0504
- 退出：窗口关闭时发 0x1002 control=0；启动未完成则跳过（用 `ratctl stop` 兜底）
- 启动/停止都用"不等待回执"发送（`screenStream(..., wait=false)`），避免退出时来不及停流

**im_ctl（C++ ImGui，`im_ctl`）** —— GUI 控制端：
- 复用 `client/core` 协议栈（protocol/crypto/net），`ctl_client`（CONTROL 客户端：连接/读线程/sendAndWait/业务命令/屏幕流/输入事件）
- 界面：Sessions 表格（点击选中）、Control（sysinfo/exec/stream 开关/**hook 独立开关**）、Screen（WIC 解码 JPEG → GL 纹理显示）
- 命令后台化（`run_cmd_async` + RAII Guard，UI 不阻塞）；`sendRaw` 发送锁（多线程安全）
- 键鼠 LL hook（过滤注入防回环）→ 批量发 0x0504；Docking 布局；窗口状态持久化（`ctl_window.ini`）

---

## 5. 关键流程时序

### 5.1 Agent 上线（长连接）
```
Agent ──HELLO(明文, role=AGENT+agent_id+nonce)──▶ C2
C2 ──HELLO_ACK(server_nonce)──▶ Agent
双方派生会话密钥 → C2 注册会话（重复 id 踢旧）→ Agent 心跳 15s
```

### 5.2 业务命令（sysinfo，长连接）
```
ctl ──SysInfo(0x3001, agent_id)──▶ C2.ctlLoop
C2 ──直发(seq=NextSeq)──▶ Agent 连接
Agent cmd_dispatch → collect_sysinfo → 回 CMD_RESULT(seq=回显)
C2 agentLoop → relay.Take(agent_id, seq) → 回传 ctl（seq 还原 cliSeq）
```

### 5.3 屏幕流全链路
```
ctl: use agent → start stream ──ScreenStream(0x1002)──▶ C2 ──▶ Agent
Agent cmd_dispatch → start_screen_stream → 连 4445 → StreamHello/Ack → stream_key
Agent 帧循环: grab → WIC JPEG → 加密 → 4445
C2: 握手 → RegisterAgent(配对 ctl) → 收帧 → 转 ctl 密钥 → 转发 ctl
ctl: 读线程收 0x0503（跳过防重放）→ WIC 解码 → GL 纹理显示
ctl: hook 采集键鼠 → 0x0504(agent_id+events) → C2 → Agent 注入
```

### 5.4 停止
```
ctl: stop stream ──ScreenStream(control=0)──▶ C2(清理配对) ──▶ Agent
Agent: stop_screen_stream（g_running=false → 线程退出 ≤5s）→ 断开 4445
C2: handleStreamConn 退出 → RemoveAgent
```

---

## 6. 安全机制

- **认证**：PSK 派生的三把密钥；屏幕流/beacon 靠"能正确加密/解密"隐式认证（无密钥者第一个包即失败断开）
- **机密性/完整性**：AES-256-GCM（每包随机 nonce + tag）
- **防重放**：请求类包 seq 严格递增（帧/回显包独立空间，避免冲突）
- **流量伪装**：beacon 信道伪装 HTML 网页、POST 伪装表单；屏幕流走独立长连接（仅激活时存在）
- **已知简化**：PSK 编译期内置（生产外部化）；握手不校验 PSK 一致性（不符到第一个加密包才暴露）

---

## 7. 目录结构与构建

```
c2/
├── docs/              # ARCHITECTURE(计划)/protocol(协议)/PROGRESS(进度)/DESIGN(本文)
├── internal/protocol/ # Go 协议层（编解码/AES-GCM/HKDF/测试向量）
├── server/            # Go c2server + session
├── ctl/cmd/ratctl/    # Go 交互式 shell
├── client/core/       # C++ Agent 全部模块
├── im_ctl/            # C++ ImGui 控制端（external: imgui/glad/glfw）
├── viewer/            # C++ Win32 远控查看器（独立 CMake 工程）
└── scripts/           # e2e_m1~m4（m4 为屏幕流验收）+ ctl_stream_probe
```

构建：
```bash
GO="C:/Users/202603/.g/go/bin/go.exe"
"$GO" build -o bin/c2server.exe ./server/cmd/c2server
"$GO" build -o bin/ratctl.exe ./ctl/cmd/ratctl
"$GO" build -o bin/ctl_stream_probe.exe ./scripts/ctl_stream_probe
# Agent（MSBuild）+ im_ctl（CMake）
MSBUILD ... client/RatAgent.vcxproj -p:Configuration=Debug -p:Platform=x64
cmake --build im_ctl/build --config Debug
# viewer（CMake，独立工程；输出 <build>/bin/viewer.exe）
cmake -S viewer -B viewer/build && cmake --build viewer/build --config Debug
```

> 无 GUI/受限环境下（例如沙箱会话）可把构建输出改到工作区内新目录，避开已存在的
> VS 输出目录（其 ACL 可能禁止写入）：
> ```bash
> MSBUILD client/RatAgent.vcxproj -p:Configuration=Debug -p:Platform=x64 \
>   -p:IntDir=<repo>/build/agent/obj/ -p:OutDir=<repo>/build/agent/bin/
> GOCACHE=<repo>/.gocache go build -o build/bin/ratctl.exe ./ctl/cmd/ratctl
> cmake -S viewer -B build/viewer && cmake --build build/viewer --config Debug
> ```
> `build/`、`.gocache/`、`bin/` 均已在 `.gitignore` 中忽略。

## 8. 验证

```bash
python scripts/e2e_m4.py   # 屏幕流全链路（长连接 agent + probe：帧 JPEG 到达/输入事件/停止）
python scripts/e2e_m3.py   # beacon 轮询 + 交互式 ratctl
```

### 8.1 本轮人工端到端验证记录（beacon agent + ratctl + viewer）

环境：`c2server.exe` + `RatAgent.exe 127.0.0.1 8080 -s 5`（beacon 信道）+ `ratctl`（脚本化 stdin）。

| 项 | 命令 | 结果 |
|---|---|---|
| 会话列表/详情 | `sessions` / `session <id>` | ✅ agent 上线，详情字段正确 |
| 系统信息 | `sysinfo` | ✅ OS/CPU/内存/磁盘/主机名/显卡 |
| 截屏 | `screenshot <id> shot.jpg` | ✅ 257153 字节 JPEG（`FF D8 FF E0`），未做人工查看 |
| payload 上传执行 | `payload C:\Windows\System32\cmd.exe cmd_probe.exe --args /c echo payload-ok > ...` | ✅ 2 块（289792B）→ `executed pid=6924`，标记文件内容 `payload-ok` |
| 屏幕流 + viewer | `screen <id> 25 70 50` | ✅ viewer 拉起 → 0x1002 → agent 连 4445（paired=true）→ 190 帧（1280×720，scale=50%） |
| 输入下发 | viewer 钩子采集 | ✅ C2 日志 `recv 0x0504` + `relayed input events` |
| 正常停流 | 关闭 viewer 窗口 | ✅ 发 0x1002 control=0 → `[stream] removed stream connection` |
| 兜底停流 | `stop` | ✅ `screen stream stopped` → agent 断开 4445 |
| 踢下线 | `kill <id>` | ✅ 返回 OK（beacon 会话无 TCP 可关，见 §9） |

## 9. 已知限制与待办

- im_ctl 输入回放需 GUI 手动端到端确认
- e2e_m1/m2 为直连模型历史脚本（可归档）
- **`kill <id>` 对 beacon 会话不生效**：beacon 会话没有 TCP 连接可关（`session.Close()` 空操作），
  实际下线依赖 3×sleeptime 离线判定；要真正移除会话需服务端补"beacon 会话标记下线并删除"
- **payload 落盘不做随机化/自清理**：固定 `%TEMP%\c2_payload\<name>`，同名覆盖；
  需要"一次一清"的话应在后续版本加清理策略
- **viewer 被强杀不会停流**：异常退出（任务管理器/`taskkill /F`）不会发送 control=0，
  需 `ratctl stop` 兜底（正常关窗会自动停流）
- **viewer 输入策略**：鼠标仅图像矩形内下发，键盘仅 viewer 为前台窗口时下发；
  本机注入事件被 `LL*HF_INJECTED` 过滤，不会回环
- 正式化：PSK 外部化、多显示器/区域抓屏配置、屏幕流码率自适应
- **信道语义**：未知指令两信道一致（都回 `CMD_RESULT(status=1)`）；
  beacon 结果块允许零长度结果（`decodeResults` 用 `io.ReadFull`，`ln==0` 不读）
- `im_ctl/external/imgui` 为内嵌 git 仓库（gitlink），clone 需单独处理
