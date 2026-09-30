// input_replay.cpp — 输入事件回放（真实注入），对照逆向 sub_10006A80
#include "input_replay.h"

#include "desktop_switch.h"

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

namespace c2 {

namespace {

float g_input_scale = 1.0f;  // = ScreenSpy 的抓屏缩放


// 注入一条鼠标事件（dx/dy 为 0..65535 绝对坐标）
bool send_mouse(DWORD flags, int abs_x, int abs_y, DWORD mouse_data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = abs_x;
    in.mi.dy = abs_y;
    in.mi.mouseData = mouse_data;
    in.mi.dwFlags = flags;
    in.mi.dwExtraInfo = 0;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

// 注入一条键盘事件（对应原 keybd_event；SendInput 为等价替代）
bool send_key(WORD vk, DWORD flags) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = flags;
    in.ki.dwExtraInfo = 0;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

}  // namespace

int replay_input_events(const InputEvent* events, size_t count) {
    if (!events || count == 0)
        return -1;

    // 对应 sub_10006A80 开头的 f_SwitchThreadToInpurDesktop()：
    // 把调用线程切换到当前用户正在使用的活动桌面，注入才作用到用户看到的桌面
    switch_thread_to_input_desktop();

    // 注入期间锁定物理键鼠，防止用户干扰（对应原代码的 BlockInput 包裹语义）
    BlockInput(TRUE);

    // 屏幕像素分辨率：原代码取 GetDeviceCaps(118/117)（HORZSIZE/VERTSIZE，物理毫米），
    // 但事件坐标是像素，绝对坐标换算必须用像素分辨率 HORZRES/VERTRES 才准确。
    HDC screen = GetDC(nullptr);
    // 换算基准必须与抓屏缩放一致：帧坐标(缩放后) → 真实屏幕
    int screen_w = (int)(GetDeviceCaps(screen, HORZRES) * g_input_scale);
    int screen_h = (int)(GetDeviceCaps(screen, VERTRES) * g_input_scale);
    ReleaseDC(nullptr, screen);
    if (screen_w <= 1) screen_w = 2;
    if (screen_h <= 1) screen_h = 2;

    int injected = 0;
    for (size_t i = 0; i < count; ++i) {
        const InputEvent& ev = events[i];

        // 像素坐标 → 绝对坐标（0..65535），对应原 v9/v11 换算
        int abs_x = (int)(65535.0 / (double)(screen_w - 1) * (double)ev.x);
        int abs_y = (int)(65535.0 / (double)(screen_h - 1) * (double)ev.y);

        if (ev.msg > 0x200) {
            // --- 鼠标消息（对应原 switch v10）---
            switch (ev.msg) {
                case WM_LBUTTONDOWN:  // 0x201
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_LBUTTONUP:    // 0x202
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_LBUTTONDBLCLK:  // 0x203 左键双击 → 两次单击
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_RBUTTONDOWN:  // 0x204 右键按下 → 同时发弹起（模拟完整右键单击，对应原 goto LABEL_23）
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_RIGHTDOWN, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_RIGHTUP, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_RBUTTONUP:    // 0x205
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_RIGHTUP, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_RBUTTONDBLCLK:  // 0x206 右键双击 → 两次右键单击
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_RIGHTDOWN, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_RIGHTUP, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_RIGHTDOWN, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_RIGHTUP, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_MBUTTONDOWN:  // 0x207
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MIDDLEDOWN, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_MBUTTONUP:    // 0x208
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MIDDLEUP, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_MBUTTONDBLCLK:  // 0x209 中键双击 → 两次中键单击
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MIDDLEDOWN, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MIDDLEUP, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MIDDLEDOWN, abs_x, abs_y);
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MIDDLEUP, abs_x, abs_y);
                    ++injected;
                    break;
                case WM_MOUSEWHEEL:   // 0x20A 滚轮
                    send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_WHEEL, abs_x, abs_y,
                               (DWORD)(short)ev.wheel);
                    ++injected;
                    break;
                default:
                    break;
            }
        } else if (ev.msg == WM_MOUSEMOVE) {  // 0x200 鼠标移动
            send_mouse(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE, abs_x, abs_y);
            ++injected;
        } else {
            // --- 键盘消息（对应原 switch v10 < 0x200 分支）---
            switch (ev.msg) {
                case WM_KEYDOWN:      // 0x100
                case WM_SYSKEYDOWN: { // 0x104
                    WORD vk = (WORD)ev.vk;
                    // 方向键需扩展键标志（对应原 v12 == 37/39/38/40 判断）
                    bool ext = (vk == VK_LEFT || vk == VK_RIGHT ||
                                vk == VK_UP || vk == VK_DOWN);
                    send_key(vk, ext ? KEYEVENTF_EXTENDEDKEY : 0);
                    ++injected;
                    break;
                }
                case WM_KEYUP:        // 0x101
                case WM_SYSKEYUP: {   // 0x105
                    WORD vk = (WORD)ev.vk;
                    bool ext = (vk == VK_LEFT || vk == VK_RIGHT ||
                                vk == VK_UP || vk == VK_DOWN);
                    send_key(vk, (ext ? KEYEVENTF_EXTENDEDKEY : 0) | KEYEVENTF_KEYUP);
                    ++injected;
                    break;
                }
                default:
                    break;
            }
        }
    }

    // 注入结束，解锁物理输入
    BlockInput(FALSE);
    return injected;
}

void set_input_scale(float scale) {
    g_input_scale = (scale > 0.0f) ? scale : 1.0f;
}

}  // namespace c2
