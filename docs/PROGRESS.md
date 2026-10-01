# 项目进度记录（PROGRESS）

> 最后更新：2026-09-30。供下次快速恢复工作状态。
> 本文件记录**本仓库（`D:\code\projects\c2`）**的实际状态。

## 1. 项目概览

`c2/` —— 基于《ARCHITECTURE.md》的 C2 远控系统（已按 DESIGN.md 演进）：
- **Agent**：C++（`c2/client/core`）。**当前 `main()` 默认入口是 `run_beacon`（HTTP 轮询 8080）**；
  长连接模式 `run_agent`（TCP 4444）在 `main.cpp` 中保留，切换只需改 `main()` 里那一行
- **Server**：Go（`c2/server/cmd/c2server`），TCP 4444（ctl 信道）+ HTTP 8080（beacon）+ **TCP 4445（屏幕流中继）**
- **CTL**：`ratctl`（Go 交互式 shell）+ `im_ctl`（ImGui 控制端）+ **`viewer`（C++ Win32 远控查看器，新）**

## 2. 已完成功能

| 功能 | 状态 | 验证 |
|---|---|---|
| 协议层（20B 头 + AES-GCM + HKDF + 向量） | ✅ | go test |
| beacon 信道（GET 命令/POST 结果/伪装 HTML） | ✅ | e2e_m3 + 本轮 e2e |
| 命令分发中心（SYS_INFO/EXEC_PS） | ✅ | e2e_m2/m3 + 本轮 e2e |
| ratctl 交互式 shell（meterpreter 风格） | ✅ | e2e_m3 |
| **屏幕流（长连接 4445）**：ScreenSpy→WIC JPEG→中继→ctl | ✅ | e2e_m4 + 本轮 e2e |
| 输入事件下行（ctl→c2server→agent 注入） | ✅ | 本轮 e2e（C2 日志 relayed input） |
| im_ctl（ImGui：会话/命令/屏幕显示/输入回放） | ✅ 编译+界面 | 未端到端跑 GUI |
| **ratctl 命令补全**：`session`/`kill`/`screenshot`/`screen`/`stop`/`payload` | ✅ | 本轮 e2e |
| **远程截屏 0x1001**（GDI 抓帧 + WIC JPEG） | ✅ | 本轮 e2e（257 KB JPEG） |
| **payload 上传执行 0x3009**（分块→%TEMP%→CreateProcess） | ✅ | 本轮 e2e（真实执行 + 标记文件） |
| **viewer（C++ Win32 查看器）** | ✅ | 本轮 e2e（190 帧 + 输入下发 + 停流） |
| 本地 git 版本管理（VS 专用 .gitignore） | ✅ | 见 §4 |

## 3. 关键架构（屏幕流 + payload）

```
Agent(beacon 8080 / 长连接 4444) ──命令──▶ c2server ◀──ctl(4444)
Agent ──4445 屏幕流连接──▶ c2server ──帧(转换加密)──▶ ctl 中配对的那条连接
ctl ──输入事件 0x0504──▶ c2server ──转发──▶ Agent(4445) → SendInput 注入
ctl ──payload 0x3009（分块）──▶ c2server（透明转发）──▶ Agent：写 %TEMP%\c2_payload\ → CreateProcess
```

- 命令流转：ctl 发业务指令（payload 首 16B = agent_id）→ c2server 剥离前缀后按 agent 模式
  （beacon 入队 / 长连接直发）下发 → Agent `cmd_dispatch` 处理
- 帧：`CmdScreenFrame(0x0503)` = seq+w+h+jpeg_len+JPEG（stream_key 加密）
- 输入：`CmdInputEvents(0x0504)` = agent_id(16)+count+InputEvent[24B]（ctl→C2 带前缀，C2 去前缀转发）
- **屏幕流配对关键点**：C2 把帧发给"**发起 0x1002 的那条 ctl 连接**"，
  所以流必须由 viewer 自己发起；ratctl 的 `screen` 只负责拉起 viewer 进程
- payload 分块格式见 `docs/protocol.md` §10；ctl 中间块流水线发送、只等末块结果

## 4. Git 提交历史（本仓库，均可回退）

```
907d636 feat: viewer 远程查看器 + ctl_client 插件/wait 参数 + 文档同步
66f5d66 feat: 协议扩展(0x1001 截屏/0x3009 payload) + Agent 实现 + ratctl 命令补全
27e49f4 chore: 初始化仓库（VS 专用 .gitignore，仅托管源码与文档）
```

## 5. 快速启动（下次开工）

```powershell
# 0) 先杀残留进程（重要！端口占用会静默失败）
taskkill /F /IM c2server.exe 2>$null; taskkill /F /IM RatAgent.exe 2>$null; taskkill /F /IM viewer.exe 2>$null
netstat -ano | findstr ":4444 :4445 :8080"

# 1) Go（本机 go 在 PATH：C:\Users\Mutsumi\.g\go\bin\go.exe）
$env:GOCACHE='D:\code\projects\c2\.gocache'      # 沙箱/受限环境必须指向可写目录
go build -o build/bin/c2server.exe ./server/cmd/c2server
go build -o build/bin/ratctl.exe   ./ctl/cmd/ratctl
go test ./...

# 2) Agent（VS2022/18 MSBuild；输出到工作区 build/，避开既有 VS 目录 ACL）
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  client\RatAgent.vcxproj -p:Configuration=Debug -p:Platform=x64 `
  -p:IntDir=D:/code/projects/c2/build/agent/obj/ -p:OutDir=D:/code/projects/c2/build/agent/bin/

# 3) viewer（CMake，独立工程）
cmake -S viewer -B build/viewer
cmake --build build/viewer --config Debug        # 产物 build/viewer/bin/viewer.exe

# 4) 运行联调（beacon 模式：Agent 连 8080，-s 为轮询秒数）
.\build\bin\c2server.exe
.\build\agent\bin\RatAgent.exe 127.0.0.1 8080 -s 5
.\build\bin\ratctl.exe                           # 交互式：sessions → use <id> → ...
```

ratctl 命令集（`help` 可查）：
`sessions / use <id> / back / session <id> / kill <id> / sysinfo / exec <ps> /
screenshot [id] <file> / screen [id] [fps] [quality] [scale] / stop / payload <file> [name] [--args <参数>]`

## 6. 剩余待办

1. **im_ctl GUI 手动端到端**：启动 server+agent+im_ctl，选 agent → start stream 看屏幕 →
   移动鼠标验证回放注入（hook 已实现；本轮只验证了 viewer 这条等价链路）
2. **`kill <id>` 对 beacon 会话不生效**：服务端 `CtlKillSession` 只 `Close()` 连接，
   beacon 会话没有 TCP 连接；需要服务端补"标记下线并从注册表删除"
3. **payload 落盘策略**：当前固定 `%TEMP%\c2_payload\<name>`，同名覆盖、不自动清理
4. **e2e_m1/m2 过时**：直连模型历史脚本，可归档
5. 正式化：PSK 外部化、多显示器/区域抓屏配置、屏幕流码率自适应

## 7. 已知注意点

- **构建目录 ACL**：`bin/`、`client/x64/`、`client/RatAgent/x64/`、`im_ctl/build/` 等
  既有目录在受限（沙箱）会话下**不可写**；受限环境一律输出到工作区内新目录
  `build/`、`.gocache/`（已在 .gitignore）
- **Agent 默认入口**：`client/core/main.cpp` 的 `main()` 当前调用 `run_beacon`（HTTP 8080）。
  要用长连接模式，把 `int rc = run_beacon(argc, argv);` 改成 `run_agent(host, port)`
- **测试用后台进程**：用 `Start-Process` 或工具前台上限内启动的进程可能在命令结束时被回收；
  长跑进程用受管后台作业启动
- **屏幕流参数已生效**：`fps/quality/scale` 全部按 0x1002 参数工作（截屏 0x1001 质量为 75）；
  帧节奏由 `next_frame` 时间片控制，输入轮询最多占用等待下一帧的空档（≤50ms）
- **屏幕流线程可自愈**：连接断开时线程自行退出并清标志，之后可直接再次 start（无需先 stop）
- **viewer 强杀不停流**（不会发 control=0）→ 用 `ratctl stop` 兜底
- **屏幕流帧的 seq 是 agent 侧递增**；ctl 端读线程对 `0x0503` 帧跳过防重放（独立 seq 空间）
- **业务指令 plugin 字段**：C2 当前只按 cmd 段路由，plugin 不符也能工作；
  新代码已按规范填正确 plugin（system=3 / remote=1），后续插件化时不要依赖旧行为
- `im_ctl/external/imgui`、`im_ctl/external/glfw`、`im_ctl/external/glad` 是**第三方目录**
  （imgui 内含独立 .git），已在 .gitignore 中整体忽略，clone 后需单独获取

## 8. 交付前只读 review 结论（子代理静态审查）

### 8.1 第一轮已修复

| 级别 | 位置 | 问题 | 修复 |
|---|---|---|---|
| 高 | `im_ctl/ctl_client.cpp` 帧分发 | `body.size() >= 12 + jl` 在 32 位回绕 → 越界读，可达 `memcpy(NULL, jpeg, 4GiB)` 崩溃（帧内 jpeg_len 由被控端提供，C2 不校验） | 改为 `jl <= body.size() - 12` |
| 高 | `im_ctl/ctl_client.h/.cpp` 待响应表 | `Pending` 为栈对象、map 存裸指针；超时返回与迟到响应竞争 → use-after-free | 改为 `std::shared_ptr<Pending>` |
| 中 | `viewer/main.cpp` 输入 | 钩子给桌面坐标，图像矩形是客户区坐标，缺 `ScreenToClient` → 鼠标坐标整体偏移 | flush 前 `ScreenToClient` |
| 中 | `ctl/cmd/ratctl/viewer_launch.go` | 候选路径与 CMake 实际输出（`<build>/bin/viewer.exe`）不匹配 | 补 `viewer/build/bin`、`build/viewer/bin` 等候选 |
| 低 | `im_ctl/ctl_client.cpp` listSessions | `rd32() \| ((uint64_t)rd32()<<32)` 未定序 UB（且都改 `off`）→ last_seen 错值 | 拆成两条语句 |
| 低 | `im_ctl/ctl_client.cpp` JPEG 解码 | `GlobalAlloc/GlobalLock` 未判空、解码尺寸未校验 → OOM/超大图崩溃 | 判空 + 1 亿像素上限 + 输入长度上限 |
| 低 | `client/core/cmd_dispatch.cpp` | 错误码在 `CloseHandle` 之后取；`SetFilePointerEx` 未检查 | 取码前移 + 检查返回值 |
| 低 | `ctl/cmd/ratctl/payload.go` / `main.go` | `--args` 超 64KiB 会被 u16 截断；分块常量与 packet.go 重复 | 增加长度校验；改用 `protocol.PayloadChunkSize` |
| 低 | `viewer/main.cpp` | `g_startupDone` 只写不读、`g_frameCount` 非原子；关窗早于启动线程发流会漏停 | 删除残留标志、改 `std::atomic`、先 `join` 再决定是否发停止 |
| 低 | `docs/protocol.md` §3.3 | 文档写 0x4002/0x4003 payload 为 16B agent_id，实际是 32B hex 字符串 | 更正文档 |

### 8.2 第二轮修复（本次全部完成）

第一轮 review 遗留的 6 项既有问题已全部修复，另在验证中**新发现并修复 1 项**：

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| 1 | `im_ctl/ctl_client.cpp` | seq 在发送锁外分配 → 并发发送时 seq 与落网顺序不一致，被 C2 防重放丢包；读线程 `h.seq <= lastSeq_` 单调闸门会永久吞掉"晚到的较早响应" | `sendRaw` 改为在发送锁内分配 seq；`sendAndWait` 的"分配 seq + 登记 pending + 发送"合并到同一锁；读线程改为**先按 seq 找等待者投递**，仅对无人认领的包走防重放闸门 |
| 2 | `client/core/screen_stream.cpp` | `fps/quality` 形参完全不生效（帧间隔硬编码 25fps、JPEG 质量硬编码 70；`encode_jpeg_bgra` 从不设置 WIC 质量属性 → 截屏 0x1001 的 75 也无效）；输入轮询的 50ms 阻塞还挤占帧节奏 | 帧节奏改为**基于 `next_frame` 的时间片**；JPEG 质量经属性包在 `frame->Initialize` 之前写入；`fps/quality/scale` 全部生效 |
| 3 | `client/core/screen_stream.cpp` | 先置 `g_running=true` 再建线程（失败则永久"已在运行"）；线程自行退出不清标志（断开后无法重启）；stop 等 5s 超时仍关句柄置空 → 可能并存两条流线程 | 新增 `g_thread_alive` + `ThreadGuard`（任何退出路径都清标志）；start 先占位、建线程失败回滚；stop 超时**保留句柄**，由 `reap_finished_thread()` 在线程退出后回收 |
| 4 | `client/core/screen_stream.cpp` | 50ms 轮询直接读包，超时丢半包 → 字节流错位、输入事件静默丢失 | 新增 `InputStream`：**自持重装缓冲** + `select` 短等待，只从缓冲切完整包，半包留待下次 |
| 5 | `client/core/main.cpp`、`beacon.cpp` | 未知指令两信道行为不一致（长连接不回包 → ctl 空等 240s 超时；beacon 回 status=1） | 长连接也回 `CMD_RESULT(status=1)`，与 beacon 及协议 §2 一致 |
| 6 | `server/cmd/c2server/main.go` | HELLO_ACK 的 `server_time` 用 BigEndian，与"统一小端"文档冲突 | 改 LittleEndian（无解析方，行为无影响） |
| 7 | `server/cmd/c2server/beacon.go` | **验证中新发现**：`decodeResults` 对零长度结果调用 `r.Read(空切片)`，`bytes.Reader` 在缓冲区末尾返回 `io.EOF` → 整个结果块解码失败，Agent 反复 `POST failed`（任何"status 非 0 且结果为空"的结果都会触发，未知指令即此类） | `ln > 0` 才读结果，并改用 `io.ReadFull`（避免短读静默截断） |

### 8.3 第二轮验证证据（beacon agent + 临时探针，均为实测）

| 项 | 证据 |
|---|---|
| server_time 小端 | 探针按小端解析 = 本机 `UnixMilli`，drift=0ms |
| fps 生效 | 请求 6 / 8 / 5 → 实测 6.1 / 8.1 / 5.2 fps（修复前恒为"25fps 减去 50ms 轮询开销" ≈ 7–10） |
| quality 生效 | 同 scale 下：quality 25 → 52.8KB、40 → 37KB、80 → 90.5KB、90 → 157.5KB |
| scale 生效 | 50% → 1280×720、40% → 1024×576、30% → 768×432 |
| 输入下行 + 重装缓冲 | 探针发 0x0504 → C2 `relayed input events` → Agent `injected 1 input events` |
| 未知指令 | `0x3099` → `CMD_RESULT status=1`（不再空等；同时验证第 7 项修复） |
| 线程自行退出后重启 | 杀掉 C2 → Agent `input channel broken` → `stopped`；重启 C2 后再 start → `screen stream started`（修复前会返回"已在运行"） |

> 本轮验证用临时探针（`build/e2e/probe.go`，gitignored；验证后已移入回收站）。
