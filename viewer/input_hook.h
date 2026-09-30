// input_hook.h — viewer 的键鼠采集（低级钩子，协议 §9.3）
#pragma once

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <vector>

#include "protocol.h"

namespace viewer {

// 安装/卸载 WH_MOUSE_LL + WH_KEYBOARD_LL（须在拥有消息循环的线程调用）
bool install_hooks();
void uninstall_hooks();
bool hooks_active();

// 取出并清空已采集的事件（线程安全）
std::vector<c2::InputEvent> take_events();
void clear_events();

// 绘制代码更新：客户区内的图像矩形 + 当前帧尺寸（用于坐标换算）
void set_image_rect(int left, int top, int right, int bottom);
void set_frame_size(int w, int h);
bool image_ready();

// 本地屏幕坐标 → 远程帧坐标（图像矩形之外返回 false）
bool map_to_remote(int lx, int ly, int& rx, int& ry);

}  // namespace viewer
