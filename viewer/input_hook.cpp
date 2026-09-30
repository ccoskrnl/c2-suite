// input_hook.cpp — 键鼠低级钩子采集（复用 im_ctl/main.cpp 的采集逻辑）
//
// 采集范围与过滤（与 im_ctl 一致）：
//   - 鼠标：全局采集，但只在"图像矩形内"的事件才转换为远程坐标后发送
//   - 键盘：仅 viewer 窗口为前台时采集
//   - 过滤 LLMHF_INJECTED / LLKHF_INJECTED，避免把本机注入事件回环发回远端
#include "input_hook.h"

#include <mutex>

namespace viewer {

namespace {

HHOOK g_mouseHook = nullptr;
HHOOK g_kbdHook = nullptr;
uint32_t g_evSeq = 0;

std::mutex g_evMx;
std::vector<c2::InputEvent> g_events;

std::mutex g_imgMx;
int g_imgL = 0, g_imgT = 0, g_imgR = 0, g_imgB = 0;  // 客户区坐标系下的图像矩形
int g_frameW = 0, g_frameH = 0;

LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* ms = (MSLLHOOKSTRUCT*)lp;
        if (!(ms->flags & LLMHF_INJECTED)) {
            c2::InputEvent ev{};
            ev.seq = ++g_evSeq;
            ev.msg = (uint32_t)wp;  // WM_MOUSEMOVE / 按键 / 滚轮
            ev.x = ms->pt.x;
            ev.y = ms->pt.y;
            if (wp == WM_MOUSEWHEEL)
                ev.wheel = (int16_t)HIWORD(ms->mouseData);
            std::lock_guard<std::mutex> lk(g_evMx);
            if (g_events.size() < 512)
                g_events.push_back(ev);
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

LRESULT CALLBACK kbd_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* kb = (KBDLLHOOKSTRUCT*)lp;
        if (!(kb->flags & LLKHF_INJECTED)) {
            c2::InputEvent ev{};
            ev.seq = ++g_evSeq;
            ev.msg = (uint32_t)wp;  // WM_KEYDOWN/UP/SYSKEYDOWN/SYSKEYUP
            ev.vk = kb->vkCode;
            std::lock_guard<std::mutex> lk(g_evMx);
            if (g_events.size() < 512)
                g_events.push_back(ev);
        }
    }
    return CallNextHookEx(g_kbdHook, code, wp, lp);
}

}  // namespace

bool install_hooks() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!g_mouseHook)
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, hInst, 0);
    if (!g_kbdHook)
        g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, hInst, 0);
    return g_mouseHook != nullptr && g_kbdHook != nullptr;
}

void uninstall_hooks() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_kbdHook) {
        UnhookWindowsHookEx(g_kbdHook);
        g_kbdHook = nullptr;
    }
}

bool hooks_active() { return g_mouseHook != nullptr && g_kbdHook != nullptr; }

std::vector<c2::InputEvent> take_events() {
    std::vector<c2::InputEvent> out;
    std::lock_guard<std::mutex> lk(g_evMx);
    out.swap(g_events);
    return out;
}

void clear_events() {
    std::lock_guard<std::mutex> lk(g_evMx);
    g_events.clear();
}

void set_image_rect(int left, int top, int right, int bottom) {
    std::lock_guard<std::mutex> lk(g_imgMx);
    g_imgL = left;
    g_imgT = top;
    g_imgR = right;
    g_imgB = bottom;
}

void set_frame_size(int w, int h) {
    std::lock_guard<std::mutex> lk(g_imgMx);
    g_frameW = w;
    g_frameH = h;
}

bool image_ready() {
    std::lock_guard<std::mutex> lk(g_imgMx);
    return g_frameW > 0 && g_frameH > 0 && g_imgR > g_imgL && g_imgB > g_imgT;
}

bool map_to_remote(int lx, int ly, int& rx, int& ry) {
    std::lock_guard<std::mutex> lk(g_imgMx);
    if (g_frameW <= 0 || g_frameH <= 0 || g_imgR <= g_imgL || g_imgB <= g_imgT)
        return false;
    if (lx < g_imgL || ly < g_imgT || lx >= g_imgR || ly >= g_imgB)
        return false;
    rx = (lx - g_imgL) * g_frameW / (g_imgR - g_imgL);
    ry = (ly - g_imgT) * g_frameH / (g_imgB - g_imgT);
    return true;
}

}  // namespace viewer
