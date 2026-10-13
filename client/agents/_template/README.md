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
| 停止 | 有 `autostart.args` 用文件参数，没有就用内置默认配置（加载即自启） |
