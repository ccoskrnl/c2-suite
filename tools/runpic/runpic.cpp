// runpic.cpp — PIC blob 执行器（反射加载验证 harness）
//
// 用途：在当前进程内执行 Crystal Palace 产出的 position-independent blob
//       （例如 Kali 上 RfUdrl 项目 `cpl link` 出的 out.bin）。
//
//       blob 入口 go() 位于偏移 0（loader.spec 的 +gofirst）。RfUdrl 的 go()
//       会把内嵌的 DLL 完整走一遍反射加载：映射 → 重定位 → 导入 → TLS 回调/
//       异常目录/分节权限 → NtContinue 转入 DLL 入口点（DllMain,参数
//       DLL_PROCESS_ATTACH）。本程序**只负责把 blob 跑起来**，加载逻辑全在
//       blob 内部；DllMain 返回后 loader 线程经合成栈自然退出。
//
// 用法： runpic.exe <pic.bin> [--hold <seconds>]
//   --hold N   loader 线程结束后再驻留 N 秒，给加载出的 DLL 工作线程时间
//              运行（默认 60；-1 表示无限驻留，Ctrl+C / 进程终止即结束）。
//
// 注意：
//   - 本进程就是宿主：进程退出 = 加载出的所有线程随之终止；
//   - 若要加载的 RatAgent 自动上线，先写好 autostart 触发器再运行本程序：
//     %LOCALAPPDATA%\RatAgent\autostart.args（首行 = 参数串，如
//     "127.0.0.1,8080,-s,2,id=...,log=..."），DllMain 检测到即自建线程启动；
//   - 无控制台时 DLL 会自己把日志落到 %LOCALAPPDATA%\RatAgent\agent.log。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

    bool read_all_file(const wchar_t* path, std::vector<uint8_t>& out, std::string& err)
    {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            err = "CreateFileW failed (GetLastError=" + std::to_string(GetLastError()) + ")";
            return false;
        }
        LARGE_INTEGER sz = { 0 };
        if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (256LL << 20))
        {
            CloseHandle(f);
            err = "bad file size";
            return false;
        }
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        const BOOL ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr);
        CloseHandle(f);
        if (!ok || got != out.size())
        {
            err = "ReadFile failed";
            return false;
        }
        return true;
    }

    void usage()
    {
        printf("usage: runpic.exe <pic.bin> [--hold <seconds>]\n"
               "  --hold N   hold the process N seconds after the loader thread exits\n"
               "             (default 60; -1 = forever)\n");
    }

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        usage();
        return 2;
    }
    const wchar_t* pic_path = argv[1];
    int hold_s = 60;
    for (int i = 2; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--hold") == 0 && i + 1 < argc)
            hold_s = _wtoi(argv[++i]);
        else
        {
            printf("[!] unknown arg: %ls\n", argv[i]);
            usage();
            return 2;
        }
    }

    std::vector<uint8_t> blob;
    std::string err;
    if (!read_all_file(pic_path, blob, err))
    {
        printf("[!] read %ls failed: %s\n", pic_path, err.c_str());
        return 1;
    }
    printf("[*] pic: %ls (%zu bytes)\n", pic_path, blob.size());

    // blob 自身是位置无关代码：一块 RWX 内存 + 入口在偏移 0。
    // （RfUdrl 加载 DLL 的镜像内存由 blob 内部分配并按节收紧权限，与此无关）
    void* mem = VirtualAlloc(nullptr, blob.size(), MEM_RESERVE | MEM_COMMIT,
        PAGE_EXECUTE_READWRITE);
    if (!mem)
    {
        printf("[!] VirtualAlloc failed (GetLastError=%lu)\n", GetLastError());
        return 1;
    }
    memcpy(mem, blob.data(), blob.size());
    FlushInstructionCache(GetCurrentProcess(), mem, blob.size());
    printf("[*] pic mapped at %p (RWX), entry = offset 0\n", mem);

    // 在独立线程上进入 go()：DllMain 返回后经合成栈（BaseThreadInitThunk）
    // 收尾，本线程正常退出，不影响加载出的 DLL 工作线程。
    DWORD tid = 0;
    HANDLE t = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)mem, nullptr, 0, &tid);
    if (!t)
    {
        printf("[!] CreateThread failed (GetLastError=%lu)\n", GetLastError());
        return 1;
    }
    printf("[*] loader thread %lu entered go()\n", tid);

    const DWORD wait_s = 120; // DllMain 应当亚秒级返回；只做兜底
    const DWORD w = WaitForSingleObject(t, wait_s * 1000);
    if (w == WAIT_OBJECT_0)
    {
        DWORD code = 3;
        GetExitCodeThread(t, &code);
        printf("[*] loader thread exited (code=%lu) — DLL entry point returned\n", code);
    }
    else
    {
        printf("[!] loader thread still running after %us (unexpected)\n", wait_s);
    }
    CloseHandle(t);

    printf("[*] holding %d s (loaded image threads live in this process; Ctrl+C to end)\n",
        hold_s);
    const bool forever = (hold_s < 0);
    for (int elapsed = 0; forever || elapsed < hold_s; ++elapsed)
    {
        Sleep(1000);
        if ((elapsed + 1) % 10 == 0)
            printf("[*] alive %ds\n", elapsed + 1);
    }
    printf("[*] hold finished, exiting (all loaded threads die with the process)\n");
    return 0;
}
