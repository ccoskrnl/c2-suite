// input_replay.h — 输入事件回放（真实注入），对照逆向 sub_10006A80
#pragma once

#include <cstddef>

#include "protocol.h"

namespace c2 {

// 将 count 个输入事件注入真实系统（SendInput，等价替代原 mouse_event/keybd_event）。
// 返回成功注入的事件数；参数非法或注入失败返回 -1。
int replay_input_events(const InputEvent* events, size_t count);

// 设置抓屏缩放：注入坐标换算基准 = 真实屏幕分辨率 × scale
// （必须与 ScreenSpy 的 dpi_scale_ 一致，否则注入只覆盖屏幕的一部分）
void set_input_scale(float scale);

}  // namespace c2
