// capability.cpp — 能力注册表实现（见 capability.h）
#include "capability.h"

#include "session.h"  // AgentConfig

#include <vector>

namespace c2
{

    namespace
    {
        std::vector<CapCommand> g_cmds;
        std::vector<CapConfigFn> g_configs;
        bool g_registered = false;
    }

    void cap_register_commands(const CapCommand* table, size_t count)
    {
        if (!table)
            return;
        for (size_t i = 0; i < count; ++i)
        {
            if (table[i].fn)
                g_cmds.push_back(table[i]);
        }
    }

    void cap_register_config(CapConfigFn fn)
    {
        if (fn)
            g_configs.push_back(fn);
    }

    void cap_ensure_registered()
    {
        if (g_registered)
            return;
        g_registered = true; // 先置位：注册过程中重入也不会重复登记
        register_module_capabilities();
    }

    bool cap_dispatch(uint16_t cmd, const uint8_t* payload, size_t len,
        std::vector<uint8_t>& response, uint8_t& status)
    {
        for (const auto& c : g_cmds)
        {
            if (c.cmd == cmd)
                return c.fn(cmd, payload, len, response, status);
        }
        return false;
    }

    void cap_notify_config(const std::string& host, const AgentConfig& cfg)
    {
        for (CapConfigFn fn : g_configs)
            fn(host, cfg);
    }

}  // namespace c2
