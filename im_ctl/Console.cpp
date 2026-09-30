#include "Console.h"
#include <cstdarg>
#include <algorithm>
#include <iostream>

Console::Console()
{
    ClearLog();
    memset(m_inputBuf, 0, sizeof(m_inputBuf));
    m_historyPos = -1;

    RegisterCommand("help", [this](const std::string &)
                    {
        AddLog("[help] available commands:");
        for (auto& cmd : m_commands)
            AddLog("  " + cmd.first); });
    RegisterCommand("clear", [this](const std::string &)
                    { ClearLog(); });
}

void Console::ClearLog()
{
    m_logs.clear();
}

// 主要输出函数
void Console::AddLog(const std::string &text, ImVec4 color)
{
    m_logs.push_back({text, color});
    const size_t MAX_LOGS = 1000;
    if (m_logs.size() > MAX_LOGS)
        m_logs.pop_front();
    m_scrollToBottom = true;
}

// 格式化输出函数（内部转为 string 后调用 AddLog）
void Console::AddLogFmt(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    AddLog(std::string(buffer)); // 默认白色
}

void Console::RegisterCommand(const std::string &command, CommandCallback callback)
{
    m_commands[command] = std::move(callback);
}

int Console::TextEditCallbackStub(ImGuiInputTextCallbackData *data)
{
    Console *console = static_cast<Console *>(data->UserData);
    return console->TextEditCallback(data);
}

int Console::TextEditCallback(ImGuiInputTextCallbackData *data)
{
    switch (data->EventFlag)
    {
    case ImGuiInputTextFlags_CallbackHistory:
    {
        // 处理上下方向键
        if (m_history.empty())
            return 0;
        int prevHistoryPos = m_historyPos;
        if (data->EventKey == ImGuiKey_UpArrow)
        {
            if (m_historyPos == -1)
                m_historyPos = (int)m_history.size() - 1;
            else if (m_historyPos > 0)
                m_historyPos--;
        }
        else if (data->EventKey == ImGuiKey_DownArrow)
        {
            if (m_historyPos != -1)
            {
                if (++m_historyPos >= (int)m_history.size())
                    m_historyPos = -1;
            }
        }
        if (prevHistoryPos != m_historyPos)
        {
            const char *history_str = (m_historyPos >= 0) ? m_history[m_historyPos].c_str() : "";
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, history_str);
        }
        return 0;
    }
    case ImGuiInputTextFlags_CallbackCompletion:
    {
        // 简单的自动补全：输入前几个字符后按 Tab
        // 这里只做最基础的匹配，你可以扩展
        std::string current = data->Buf;
        std::vector<std::string> candidates;
        for (auto &cmd : m_commands)
        {
            if (cmd.first.find(current) == 0)
                candidates.push_back(cmd.first);
        }
        if (candidates.size() == 1)
        {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, candidates[0].c_str());
            data->InsertChars(data->BufTextLen, " ");
        }
        else if (candidates.size() > 1)
        {
            AddLog("Possible commands:");
            for (auto &c : candidates)
                AddLogFmt("  %s", c.c_str());
        }
        return 0;
    }
    }
    return 0;
}

void Console::ExecCommand(const std::string &command_line)
{
    // 忽略空命令
    if (command_line.empty())
        return;

    // 添加到历史（去重）
    if (m_history.empty() || m_history.back() != command_line)
        m_history.push_back(command_line);
    m_historyPos = -1;

    // 输出命令本身（带标记）
    AddLogFmt("> %s\n", command_line.c_str());

    // 解析命令名和参数
    size_t pos = command_line.find(' ');
    std::string cmd = command_line.substr(0, pos);
    std::string args = (pos != std::string::npos) ? command_line.substr(pos + 1) : "";

    // 查找并执行
    auto it = m_commands.find(cmd);
    if (it != m_commands.end())
    {
        it->second(args);
    }
    else
    {
        AddLogFmt("[error] unknown command: %s", cmd.c_str());
    }
}

void Console::Draw(const char *title, bool *p_open)
{
    ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, p_open))
    {
        ImGui::End();
        return;
    }

    // 日志过滤和清空按钮
    if (ImGui::Button("Clear"))
        ClearLog();
    ImGui::SameLine();
    bool copy = ImGui::Button("Copy");
    ImGui::SameLine();
    m_filter.Draw("Filter", -100.0f);

    ImGui::Separator();

    // 日志显示区域
    ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 100000.0f);

    if (copy)
        ImGui::LogToClipboard();

    for (const auto &entry : m_logs)
    {
        if (!m_filter.PassFilter(entry.text.c_str()))
            continue;
        ImGui::PushStyleColor(ImGuiCol_Text, entry.color);
        ImGui::TextUnformatted(entry.text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::PopTextWrapPos();

    if (copy)
        ImGui::LogFinish();

    // 自动滚动
    if (m_scrollToBottom || (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
    {
        ImGui::SetScrollHereY(1.0f);
        m_scrollToBottom = false;
    }

    ImGui::EndChild();
    ImGui::Separator();

    // 命令输入框
    ImGui::Text(">");
    ImGui::SameLine();
    ImGui::PushItemWidth(-1);
    bool reclaim_focus = false;
    ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackHistory |
                                      ImGuiInputTextFlags_CallbackCompletion;
    if (ImGui::InputText("##Input", m_inputBuf, sizeof(m_inputBuf), input_flags,
                         &TextEditCallbackStub, this))
    {
        ExecCommand(m_inputBuf);
        memset(m_inputBuf, 0, sizeof(m_inputBuf));
        reclaim_focus = true; // 保持焦点在输入框
    }
    ImGui::PopItemWidth();

    // 自动设置焦点
    ImGui::SetItemDefaultFocus();
    if (reclaim_focus)
        ImGui::SetKeyboardFocusHere(-1); // 聚焦到上一个控件

    ImGui::End();
}