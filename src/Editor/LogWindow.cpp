#include "Editor/LogWindow.h"

#include <imgui/imgui.h>
#include "Core/I18n.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Editor {

namespace {

const char* LevelName(Core::LogLevel level) {
    switch (level) {
        case Core::LogLevel::Debug: return "调试";
        case Core::LogLevel::Info:  return "信息";
        case Core::LogLevel::Warn:  return "警告";
        case Core::LogLevel::Error: return "错误";
        case Core::LogLevel::Fatal: return "致命";
        default:                    return "未知";
    }
}

ImVec4 LevelColor(Core::LogLevel level) {
    switch (level) {
        case Core::LogLevel::Debug: return ImVec4(0.58f, 0.62f, 0.70f, 1.0f);
        case Core::LogLevel::Warn:  return ImVec4(1.00f, 0.78f, 0.30f, 1.0f);
        case Core::LogLevel::Error: return ImVec4(1.00f, 0.40f, 0.38f, 1.0f);
        case Core::LogLevel::Fatal: return ImVec4(1.00f, 0.25f, 0.75f, 1.0f);
        default:                    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

} // namespace

LogWindow& LogWindow::GetInstance() {
    static LogWindow instance;
    return instance;
}

void LogWindow::RefreshSnapshot() {
    const std::uint64_t sequence = Core::GetLogSequence();
    if (sequence == m_snapshotSequence) return;

    m_records = Core::GetLogSnapshot();
    m_snapshotSequence = sequence;
}

bool LogWindow::PassesFilter(const Core::LogRecord& record) const {
    if (m_levelFilter > 0 && static_cast<int>(record.level) != m_levelFilter - 1) {
        return false;
    }
    if (!m_sourceFilter.empty() && record.source != m_sourceFilter) {
        return false;
    }
    if (m_search[0] != '\0' && record.text.find(m_search) == std::string::npos) {
        return false;
    }
    return true;
}

void LogWindow::Render(bool& showWindow) {
    if (!showWindow) return;

    bool recordsChanged = Core::GetLogSequence() != m_snapshotSequence;
    RefreshSnapshot();

    if (!ImGui::Begin(I18n::WindowTitle("游戏日志", "editor.game_log").c_str(), &showWindow)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button(Tr("清空"))) {
        Core::ClearLogHistory();
        m_records.clear();
        m_snapshotSequence = Core::GetLogSequence();
        recordsChanged = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox(Tr("自动滚动"), &m_autoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    const char* levelLabels[] = { Tr("全部"), Tr("调试"), Tr("信息"), Tr("警告"), Tr("错误"), Tr("致命") };
    ImGui::Combo(Tr("级别"), &m_levelFilter, levelLabels, IM_ARRAYSIZE(levelLabels));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    const char* sourcePreview = m_sourceFilter.empty() ? Tr("全部") : m_sourceFilter.c_str();
    if (ImGui::BeginCombo("来源", sourcePreview)) {
        const bool allSelected = m_sourceFilter.empty();
        if (ImGui::Selectable(Tr("全部"), allSelected)) {
            m_sourceFilter.clear();
        }
        ImGui::SetItemDefaultFocus();

        std::vector<std::string> sources;
        for (const Core::LogRecord& record : m_records) {
            if (record.source.empty()) continue;
            if (std::find(sources.begin(), sources.end(), record.source) == sources.end()) {
                sources.push_back(record.source);
            }
        }
        std::sort(sources.begin(), sources.end());
        for (const std::string& source : sources) {
            const bool selected = m_sourceFilter == source;
            if (ImGui::Selectable(source.c_str(), selected)) {
                m_sourceFilter = source;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##GameLogSearch", Tr("搜索日志消息..."), m_search, sizeof(m_search));

    ImGui::Separator();
    ImGui::BeginChild("GameLogList", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    const bool wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
    std::size_t visibleCount = 0;
    for (const Core::LogRecord& record : m_records) {
        if (!PassesFilter(record)) continue;

        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(record.level));
        ImGui::TextUnformatted(record.text.c_str());
        ImGui::PopStyleColor();
        ++visibleCount;
    }

    if (visibleCount == 0) {
        ImGui::TextDisabled(m_records.empty() ? Tr("暂无日志") : Tr("没有符合筛选条件的日志"));
    }
    if (recordsChanged && m_autoScroll && wasAtBottom) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::TextDisabled(Tr("显示 %zu / %zu 条 · 级别：%s%s"),
        visibleCount,
        m_records.size(),
        m_levelFilter == 0 ? "全部" : levelLabels[m_levelFilter],
        m_sourceFilter.empty() ? "" : (std::string(" · 来源：") + m_sourceFilter).c_str());
    ImGui::End();
}

} // namespace Editor
