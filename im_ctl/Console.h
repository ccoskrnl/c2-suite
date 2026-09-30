#pragma once
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include "imgui.h"

class Console
{
public:
    using CommandCallback = std::function<void(const std::string &)>;

    Console();
    ~Console() = default;

    // 添加日志
    void AddLog(const std::string &text, ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    void AddLogFmt(const char *fmt, ...) IM_FMTARGS(2);

    // 注册命令回调（例如：输入 "help" 时触发）
    void RegisterCommand(const std::string &command, CommandCallback callback);

    // 清空日志
    void ClearLog();

    // 每帧调用，渲染控制台窗口
    void Draw(const char *title, bool *p_open = nullptr);

private:
    struct LogEntry
    {
        std::string text;
        ImVec4 color;
    };

    std::deque<LogEntry> m_logs;                                 // 日志存储（最多保存 N 条）
    std::vector<std::string> m_history;                          // 输入历史
    int m_historyPos = -1;                                       // 当前历史位置
    char m_inputBuf[256] = {};                                   // 输入缓冲区
    bool m_autoScroll = true;                                    // 自动滚动
    bool m_scrollToBottom = false;                               // 强制滚动到底部
    ImGuiTextFilter m_filter;                                    // 日志过滤器
    std::unordered_map<std::string, CommandCallback> m_commands; // 命令表

    // 执行命令
    void ExecCommand(const std::string &command_line);
    // 输入回调（提交时）
    static int TextEditCallbackStub(ImGuiInputTextCallbackData *data);
    int TextEditCallback(ImGuiInputTextCallbackData *data);
};
