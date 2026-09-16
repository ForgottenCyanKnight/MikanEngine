#pragma once
// CommandConsoleWindow.h - 运行时命令控制台窗口（薄 UI）
// 输入行 + 历史（↑/↓）+ 输出缓冲；命令解析与执行全部在 Core::ConsoleCommands（Game.dll）。
#include "Platform/Export.h"
#include "Core/ConsoleCommands.h" // Core::ConsoleLine

#include <string>
#include <vector>

namespace Editor {

class CommandConsoleWindow {
public:
    static CommandConsoleWindow& GetInstance();

    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }

    void Render();

    // 供热键/菜单直接执行命令（输出进本窗口缓冲与日志系统）
    void ExecCommand(const std::string& line);

private:
    CommandConsoleWindow() = default;
    ~CommandConsoleWindow() = default;
    CommandConsoleWindow(const CommandConsoleWindow&) = delete;
    CommandConsoleWindow& operator=(const CommandConsoleWindow&) = delete;

    void AddLine(const std::string& text, int level);

    bool m_visible = false;
    char m_input[256] = {};
    std::vector<std::string> m_history;
    int m_historyPos = -1; // -1 = 输入行（历史游标未激活）
    bool m_scrollToBottom = false;

    struct Line { std::string text; int level; };
    std::vector<Line> m_lines;          // 本窗口输出缓冲（上限 512 行，丢最旧）
    std::vector<Core::ConsoleLine> m_execOut; // Execute 复用缓冲，避免每帧分配
};

} // namespace Editor
