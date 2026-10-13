// caps_full.cpp — "full"模块的能力清单（工程级文件，不是 core 的一部分）
//
// 每个模块工程提供一份 register_module_capabilities()，声明本产物包含哪些能力。
// core 通过 capability.h 的注册表与这些能力打交道，因此**加/减能力只改这个文件**，
// 不需要动 core（见 client/core/capability.h）。
//
// 现状（模块化阶段 2 之后）：
//   · "full" = 只剩 **系统类** 能力（sysinfo / exec / payload），它们由 core 内置处理；
//   · 远程桌面整组（抓屏 / 屏幕流 / 输入回放 / 桌面切换）已拆到
//     client/agents/screen/ 成为独立 agent（模块 id: screen）。
//   所以这里目前**不登记任何能力**——保留本文件是为了保持"每个模块工程一份清单"的结构：
//   以后要往 full 里加能力，就在这里登记。
#include "capability.h"

namespace c2
{

    void register_module_capabilities()
    {
        // 本模块没有额外的能力需要登记（系统类指令由 core 的 cmd_dispatch 直接处理）。
    }

}  // namespace c2
