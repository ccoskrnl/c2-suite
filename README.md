# C2 远控系统（rat/c2）

基于《ARCHITECTURE.md》架构设计计划书实现的可扩展 C2 远控系统。

- **客户端**：C++（Windows，core + 插件 dll，M2 起插件化）
- **服务端**：Go（C2 中转/管理服务器）
- **控制端**：Go CLI（`ratctl`）
- **协议**：自定义二进制（20B 固定头 + 分模块指令码），AES-256-GCM 加密（PSK→HKDF 派生）
- **当前状态**：M1 完成（协议 + 核心通信：握手/心跳/会话管理/离线判定）

## 目录结构

```
c2/
├── docs/protocol.md           # 协议规范 v1（两端唯一契约）
├── internal/protocol/         # Go 共享协议层（头编解码 + AES-GCM/HKDF + 向量单测）
│   └── testdata/vectors.json  # 测试向量（Go/C++ 交叉验证）
├── cmd/genvectors/            # 测试向量生成器（go run ./cmd/genvectors）
├── server/                    # Go C2 服务器
│   ├── cmd/c2server/          # 监听/握手/会话注册/心跳扫描/管理指令
│   └── internal/session/      # 会话注册表（30s 离线 / 60s 回收）
├── ctl/                       # Go CLI
│   └── cmd/ratctl/            # list 会话列表
├── client/                    # C++ Agent（VS 解决方案）
│   ├── RatAgent.sln/.vcxproj
│   └── core/                  # protocol/crypto/net/session/main
└── scripts/e2e_m1.py          # M1 端到端联调验收脚本
```

## 构建

**Go**（go 1.25+，本机路径 `C:/Users/202603/.g/go/bin/go.exe`）：

```
cd c2
go build -o bin/c2server.exe ./server/cmd/c2server
go build -o bin/ratctl.exe    ./ctl/cmd/ratctl
go test ./...                 # 协议单测（含测试向量验证）
```

**C++**（VS2022，v145 工具集）：用 VS 打开 `client/RatAgent.sln` 生成，
或命令行：

```
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" client/RatAgent.vcxproj -p:Configuration=Debug -p:Platform=x64
```

产物：`client/x64/Debug/RatAgent.exe`。

## 运行与联调（M1）

1. 启动 C2：`./bin/c2server.exe`（监听 0.0.0.0:4444，PSK 编译期内置）
2. 启动 Agent：`client/x64/Debug/RatAgent.exe 127.0.0.1 4444`
   （`--selftest` 参数可单独跑协议自检，与测试向量交叉验证）
3. 查看会话：`./bin/ratctl.exe list`
4. 自动验收：`python scripts/e2e_m1.py`（上线/在线/30s 离线判定/60s 回收，约 75s）

## 协议要点

- 数据包：`magic(0x5A5A) + version + flags + seq + timestamp + cmd + plugin + status + payload_len + payload`
- 加密：payload 用 AES-256-GCM（12B 随机 nonce），头部明文；密钥 = `HKDF-SHA256(PSK, client_nonce||server_nonce, "rat-v1")`
- 指令码分段：`0x0` 核心 / `0x1` 远程控制 / `0x2` 文件 / `0x3` 系统 / `0x4` 管理（详见 `docs/protocol.md`）

## 路线图

- **M1 ✅** 协议 + 核心通信（握手/心跳/会话/离线判定）
- **M2** 插件框架（`plugin_api.h` + 加载器）+ `plugin_remote`（截图/键鼠注入，复用旧 demo）
- **M3** `plugin_file` + `plugin_sys`（分块文件传输、进程/服务/注册表）
- **M4** 重连完善、防重放、多 CLI、打包与文档
