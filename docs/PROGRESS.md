# 项目进度记录（PROGRESS）

> 最后更新：2026-08-24。供下次快速恢复工作状态。

## 1. 项目概览

`c2/` —— 基于《ARCHITECTURE.md》的 C2 远控系统：
- **Agent**：C++（`c2/client/core`），默认入口为**长连接模式**（用户改：`main()` → `run_agent` 连 4444）；beacon 保留为 `run_beacon`（HTTP 轮询 8080）
- **Server**：Go（`c2/server/cmd/c2server`），TCP 4444（ctl 信道）+ HTTP 8080（beacon）+ **TCP 4445（屏幕流中继）**
- **CTL**：`ratctl`（Go 交互式 shell）+ `im_ctl`（ImGui 控制端，`c2/im_ctl`）

## 2. 已完成功能

| 功能 | 状态 | 验证 |
|---|---|---|
| 协议层（20B 头 + AES-GCM + HKDF + 向量） | ✅ | go test |
| beacon 信道（GET 命令/POST 结果/伪装 HTML） | ✅ | e2e_m3 |
| 命令分发中心（SYS_INFO/EXEC_PS 系统信息/PowerShell） | ✅ | e2e_m2/m3 |
| ratctl 交互式 shell（meterpreter 风格） | ✅ | e2e_m3 |
| **屏幕流（长连接 4445）**：ScreenSpy→WIC JPEG→中继→ctl | ✅ | **e2e_m4** |
| 输入事件下行（ctl→c2server→agent 注入） | ✅ | e2e_m4（发送验证） |
| im_ctl（ImGui：会话/命令/屏幕显示） | ✅ 编译+界面 | 未端到端跑 GUI |
| **im_ctl 输入回放**（LL hook 采集→0x0504） | ✅ | 编译通过，链路经 e2e_m4 验证 |

## 3. 关键架构（屏幕流）

```
Agent(4444长连接) ──命令──▶ c2server ◀──ctl(4444)
Agent ──4445 屏幕流连接──▶ c2server ──帧(转换加密)──▶ ctl(im_ctl/probe)
ctl ──输入事件 0x0504──▶ c2server ──转发──▶ Agent(4445) → SendInput 注入
```

- 命令流转：ctl 发 `RMT_SCREEN_STREAM(0x1002)` → c2server 按 agent 模式（beacon 入队 / 长连接直发）下发 → Agent `cmd_dispatch` 启动 `screen_stream`
- 帧：`CmdScreenFrame(0x0503)` = seq+w+h+jpeg_len+JPEG（stream_key AES-GCM）
- 输入：`CmdInputEvents(0x0504)` = agent_id(16)+count+InputEvent[24B]（ctl→C2 带前缀，C2 去前缀转发 agent）

## 4. Git 提交历史（最近，均可回退）

```
98d5553 c2server 屏幕流中继 + e2e_m4
19ec5d2 im_ctl：ImGui 控制端
1370a0e gitignore im_ctl 构建产物
cfb7774 Agent 屏幕流模块（ScreenSpy/WIC/长连接/注入）
41c98ba 协议：屏幕流长连接规范（§9）
b64418b ratctl 识别同步错误响应
a395bcf 修复 beacon 会话 LastSeen（反复 first GET）
20e4168 ratctl 交互式 shell 重写
```

## 5. 快速启动（下次开工）

```bash
# 构建（注意 go 用全路径，不在 PATH）
cd D:/code/cpp/rat/desktop/c2
"C:/Users/202603/.g/go/bin/go.exe" build -o bin/c2server.exe ./server/cmd/c2server
"C:/Users/202603/.g/go/bin/go.exe" build -o bin/ratctl.exe ./ctl/cmd/ratctl
"C:/Users/202603/.g/go/bin/go.exe" build -o bin/ctl_stream_probe.exe ./scripts/ctl_stream_probe
# C++ Agent（长连接模式）
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" client/RatAgent.vcxproj -p:Configuration=Debug -p:Platform=x64
# im_ctl（CMake）
cmake --build c2/im_ctl/build --config Debug   # 若 build 目录不存在：cmake -S c2/im_ctl -B c2/im_ctl/build

# 运行前先杀残留进程（重要！端口占用会静默失败）
taskkill /F /IM c2server.exe 2>nul; taskkill /F /IM RatAgent.exe 2>nul
netstat -ano | findstr ":4444 :4445 :8080"

# 端到端验收
python c2/scripts/e2e_m4.py    # 屏幕流（长连接 agent + probe，约 20s）
python c2/scripts/e2e_m3.py    # beacon + 交互式 ratctl
```

## 6. 剩余待办

1. **im_ctl GUI 手动端到端**：启动 server+agent+im_ctl，选 agent → start stream 看屏幕 → 移动鼠标验证回放注入（hook 已实现）
2. **e2e_m1/m2 过时**：直连模型历史脚本，可归档
3. **协议文档**：§9.3 输入事件说明补"ctl→C2 带 agent_id 前缀"（实现已按此）
4. 正式化：PSK 外部化、屏幕流帧率/质量参数生效（当前固定 25fps/70）

## 7. 已知注意点

- **用户工作区未提交改动**：`c2/client/main.cpp`（默认入口改长连接）、`c2/client/long.cpp`（新文件）、`c2/client/RatAgent.vcxproj`、`c2/server/cmd/c2server/main.go`（ctlLoop 长连接分支 + DropCLI）——**这些是用户改动，提交前先确认**
- `c2/im_ctl/external/imgui` 是**嵌入式 git 仓库**（gitlink），clone 后需单独处理（本地可用）
- 屏幕流 agent 端 WIC JPEG 编码 ~15-30ms/帧（2K），帧约 400KB
- **测试前必杀残留进程**（e2e 失败多半是端口占用）
- 屏幕流帧的 seq 是 agent 侧递增；ctl 端读线程对 `0x0503` 帧**跳过防重放**（与请求-响应 seq 空间独立）
