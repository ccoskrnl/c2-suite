// screen_module.h — "远程桌面"能力向 core 的注册入口
//
// 该能力目前由 screen_spy（抓帧）+ screen_stream（长连接/输入回放）实现；
// 阶段 2 会把这一组整体搬到 client/agents/screen/ 成为独立 agent。
#pragma once

namespace c2 {

// 登记本能力的指令（0x1001 截屏 / 0x1002 屏幕流开关）与配置钩子。
// 由模块工程的 register_module_capabilities() 调用（见 client/caps_full.cpp）。
void screen_module_register();

}  // namespace c2
