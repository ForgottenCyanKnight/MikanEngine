// CommandConsoleWindow.cpp - 运行时命令控制台窗口 UI
#include "Editor/CommandConsoleWindow.h"
#include "Editor/EditorUiScale.h"

#include "imgui.h"

#include <cctype>

namespace Editor {

CommandConsoleWindow& CommandConsoleWindow::GetInstance()
{
    static CommandConsoleWindow instance;
    return instance;
}

void CommandConsoleWindow::AddLine(const std::string& text, int level)
{
    m_lines.push_back({ text, level });
    if (m_lines.size() > 512)
        m_lines.erase(m_lines.begin(), m_lines.begin() + (m_lines.size() - 512));
    m_scrollToBottom = true;
}

void CommandConsoleWindow::ExecCommand(const std::string& line)
{
    std::string trimmed;
    size_t b = line.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return;
    size_t e = line.find_last_not_of(" \t\r\n");
    trimmed = line.substr(b, e - b + 1);

    AddLine("] " + trimmed, 3); // 3 = 命令回显色
    if (trimmed == "clear" || trimmed == "cls") {
        m_lines.clear();
        return;
    }
    m_execOut.clear();
    Core::ConsoleCommands::Execute(trimmed, m_execOut);
    for (const auto& l : m_execOut)
        AddLine(l.text, l.level);
}

void CommandConsoleWindow::Render()
{
    if (!m_visible) return;

    ImGui::SetNextWindowSize(ImVec2(680.0f * EditorUi::GetUiScale(), 400.0f * EditorUi::GetUiScale()),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("命令控制台", &m_visible)) {
        ImGui::End();
        return;
    }

    // 输出区
    const float inputHeight = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("##console_scroll", ImVec2(0.0f, -inputHeight), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        // 用户没有向上翻阅时才自动滚底
        const bool stickToBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        for (const Line& l : m_lines) {
            ImVec4 color;
            switch (l.level) {
                case 1: color = ImVec4(1.00f, 0.78f, 0.35f, 1.0f); break; // warn
                case 2: color = ImVec4(1.00f, 0.42f, 0.42f, 1.0f); break; // error
                case 3: color = ImVec4(0.55f, 0.75f, 1.00f, 1.0f); break; // 命令回显
                default: color = ImGui::GetStyleColorVec4(ImGuiCol_Text); break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
        if (m_scrollToBottom || stickToBottom) {
            ImGui::SetScrollHereY(1.0f);
            m_scrollToBottom = false;
        }
    }
    ImGui::EndChild();

    // 输入行
    bool reclaimFocus = false;
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    ImGui::SetNextItemWidth(-70.0f);
    if (ImGui::InputText("##console_input", m_input, sizeof(m_input), flags)) {
        std::string line(m_input);
        m_input[0] = '\0';
        if (!line.empty()) {
            m_history.push_back(line);
            m_historyPos = -1;
            ExecCommand(line);
        }
        reclaimFocus = true;
    }
    // 历史：输入框激活时 ↑/↓ 翻阅
    if (ImGui::IsItemActive() && !m_history.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
            m_historyPos = (m_historyPos == -1) ? (int)m_history.size() - 1
                                                : (m_historyPos > 0 ? m_historyPos - 1 : 0);
            snprintf(m_input, sizeof(m_input), "%s", m_history[m_historyPos].c_str());
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
            if (m_historyPos != -1) {
                ++m_historyPos;
                if (m_historyPos >= (int)m_history.size()) {
                    m_historyPos = -1;
                    m_input[0] = '\0';
                } else {
                    snprintf(m_input, sizeof(m_input), "%s", m_history[m_historyPos].c_str());
                }
            }
        }
    }
    ImGui::SetItemDefaultFocus();
    if (reclaimFocus) ImGui::SetKeyboardFocusHere(-1);

    ImGui::SameLine();
    if (ImGui::Button("清空")) {
        m_lines.clear();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("help 查看命令列表");
    }

    ImGui::End();
}

} // namespace Editor
