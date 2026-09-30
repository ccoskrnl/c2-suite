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
<待提交> docs: 同步 protocol §10 / DESIGN 验证记录 / PROGRESS / README
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
5. 正式化：PSK 外部化、屏幕流帧率/质量参数生效（当前 Agent 固定 25fps/70）、多显示器区域配置

## 7. 已知注意点

- **构建目录 ACL**：`bin/`、`client/x64/`、`client/RatAgent/x64/`、`im_ctl/build/` 等
  既有目录在受限（沙箱）会话下**不可写**；受限环境一律输出到工作区内新目录
  `build/`、`.gocache/`（已在 .gitignore）
- **Agent 默认入口**：`client/core/main.cpp` 的 `main()` 当前调用 `run_beacon`（HTTP 8080）。
  要用长连接模式，把 `int rc = run_beacon(argc, argv);` 改成 `run_agent(host, port)`
- **测试用后台进程**：用 `Start-Process` 或工具前台上限内启动的进程可能在命令结束时被回收；
  长跑进程用受管后台作业启动
- **viewer 强杀不停流**（不会发 control=0）→ 用 `ratctl stop` 兜底
- **屏幕流帧的 seq 是 agent 侧递增**；ctl 端读线程对 `0x0503` 帧跳过防重放（独立 seq 空间）
- **业务指令 plugin 字段**：C2 当前只按 cmd 段路由，plugin 不符也能工作；
  新代码已按规范填正确 plugin（system=3 / remote=1），后续插件化时不要依赖旧行为
- `im_ctl/external/imgui`、`im_ctl/external/glfw`、`im_ctl/external/glad` 是**第三方目录**
  （imgui 内含独立 .git），已在 .gitignore 中整体忽略，clone 后需单独获取
