// capability.h — 能力（模块）注册表：把"具体能力"从 core 里解耦
//
// 背景（模块化阶段 1）：core 原来**直接依赖具体能力**——
//   · cmd_dispatch.cpp 直接 include screen_spy/screen_stream，switch 里带着 0x1001/0x1002 分支
//   · agent_runtime.cpp 直接调 screen_stream_set_config()
// 结果是"加/减一个能力必须改 core"。
//
// 改成注册式之后：
//   · core 只认两样东西：**指令处理器表** + **配置钩子**（都在本文件）
//   · 每个模块工程提供自己的 caps.cpp，实现 register_module_capabilities()
//   · 加模块 = 复制模板目录 + 在自己的 caps.cpp 里登记指令，**不必改 core**
//
// 这里**没有 ABI 问题**：注册的是本进程内的普通函数指针，能力与 core 编译在同一份
// 源码里（同一模块工程）。跨进程/跨 DLL 的场景是阶段 5 的 IPC，不走这条路。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace c2 {

struct AgentConfig;

// 指令处理器：与 handle_command 同签名（payload 已剥离 16B agent_id 前缀）
using CapCommandFn = bool (*)(uint16_t cmd, const uint8_t* payload, size_t len,
                              std::vector<uint8_t>& response, uint8_t& status);

// 配置钩子：core 在建链前把连接配置交给能力（能力据此建自己的长连接/保存状态）
using CapConfigFn = void (*)(const std::string& host, const AgentConfig& cfg);

struct CapCommand {
    uint16_t cmd;
    CapCommandFn fn;
};

// ---- 供能力（模块）调用 ----
void cap_register_commands(const CapCommand* table, size_t count);
void cap_register_config(CapConfigFn fn);

// ---- 供 core 调用 ----
// 首次调用时执行本工程的 register_module_capabilities()；幂等（agent 可重启多次）
void cap_ensure_registered();
// 按注册表分发；命中并处理返回 true（未命中返回 false，调用方按"未知指令"处理）
bool cap_dispatch(uint16_t cmd, const uint8_t* payload, size_t len,
                  std::vector<uint8_t>& response, uint8_t& status);
// 通知所有已注册的配置钩子
void cap_notify_config(const std::string& host, const AgentConfig& cfg);

// ---- 各模块工程必须实现（每个工程一份，例如 client/caps_sys.cpp）----
void register_module_capabilities();

}  // namespace c2
