#include "Editor/ToolbarWindow.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "ECS/SceneECS.h"
#include "SceneSerializer.h"

namespace Editor {

ToolbarWindow& ToolbarWindow::GetInstance() {
    static ToolbarWindow instance;
    return instance;
}

void ToolbarWindow::Render() {
    // Toolbar: slightly lighter than the window background, hairline bottom border
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.135f, 0.145f, 0.17f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.05f, 0.06f, 0.08f, 1.0f));

    // 工具栏高度:紧凑(略高于默认行高,比原来 1.35x 更矮)
    float toolbarHeight = ImGui::GetFrameHeightWithSpacing() * 1.15f;
    if (ImGui::BeginChild("Toolbar", ImVec2(0, toolbarHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // 工具栏内按钮:水平 padding 略宽松、垂直收紧 → 按钮更矮,按钮内文本垂直居中(消除偏下)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
        const float btnH = ImGui::GetFrameHeight(); // 压紧 FramePadding 后的帧高
        auto fitSize = [&](const char* label) -> ImVec2 {
            ImVec2 t = ImGui::CalcTextSize(label);
            return ImVec2(t.x + ImGui::GetStyle().FramePadding.x * 2.0f, btnH);
        };
        ImGui::SetCursorPosY((toolbarHeight - btnH) * 0.5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);

        // ===== 经典播放键: 播放 / 暂停 / 停止(按运行状态切换颜色) =====
        const ImVec4 btnDefault  = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        const ImVec4 btnHoverDef = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);

        // 播放键: 未运行=可点(默认色); 运行中=绿色高亮(禁用点击)
        ImGui::PushStyleColor(ImGuiCol_Button,        m_isGameRunning ? ImVec4(0.16f, 0.55f, 0.27f, 1.0f) : btnDefault);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, m_isGameRunning ? ImVec4(0.20f, 0.62f, 0.32f, 1.0f) : btnHoverDef);
        ImGui::BeginDisabled(m_isGameRunning);
        if (ImGui::Button("播放", fitSize("播放"))) {
            m_isGameRunning = true;
            m_isGamePaused = false;
            printf("运行游戏\n");
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 4);

        // 暂停键: 未运行=禁用; 运行中可点; 暂停中=黄色高亮
        ImGui::PushStyleColor(ImGuiCol_Button,        m_isGameRunning && m_isGamePaused ? ImVec4(0.85f, 0.65f, 0.15f, 1.0f) : btnDefault);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, m_isGameRunning && m_isGamePaused ? ImVec4(0.95f, 0.72f, 0.20f, 1.0f) : btnHoverDef);
        ImGui::BeginDisabled(!m_isGameRunning);
        if (ImGui::Button("暂停", fitSize("暂停"))) {
            m_isGamePaused = !m_isGamePaused;
            printf(m_isGamePaused ? "游戏暂停\n" : "继续游戏\n");
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 4);

        // 停止键: 未运行=禁用; 运行中=红色高亮,点击停止
        ImGui::PushStyleColor(ImGuiCol_Button,        m_isGameRunning ? ImVec4(0.72f, 0.27f, 0.24f, 1.0f) : btnDefault);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, m_isGameRunning ? ImVec4(0.82f, 0.33f, 0.28f, 1.0f) : btnHoverDef);
        ImGui::BeginDisabled(!m_isGameRunning);
        if (ImGui::Button("停止", fitSize("停止"))) {
            m_isGameRunning = false;
            m_isGamePaused = false;
            printf("停止游戏\n");
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 20);

        // 坐标轴按钮:缩小,刚好显示文本
        ImGui::PushStyleColor(ImGuiCol_Button, m_showGizmoAxis ? ImVec4(0.2f, 0.5f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("坐标轴", fitSize("坐标轴"))) {
            m_showGizmoAxis = !m_showGizmoAxis;
        }
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 4);

        ImGui::PushStyleColor(ImGuiCol_Button, m_currentGizmoMode == GizmoMode::Translate ? ImVec4(0.2f, 0.5f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("平移", fitSize("平移"))) {
            m_currentGizmoMode = GizmoMode::Translate;
        }
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 4);

        ImGui::PushStyleColor(ImGuiCol_Button, m_currentGizmoMode == GizmoMode::Rotate ? ImVec4(0.2f, 0.5f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("旋转", fitSize("旋转"))) {
            m_currentGizmoMode = GizmoMode::Rotate;
        }
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 4);

        ImGui::PushStyleColor(ImGuiCol_Button, m_currentGizmoMode == GizmoMode::Scale ? ImVec4(0.2f, 0.5f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("缩放", fitSize("缩放"))) {
            m_currentGizmoMode = GizmoMode::Scale;
        }
        ImGui::PopStyleColor();

        // ===== 右侧: 保存 / 重载(右对齐) =====
        {
            const float rightGroupW = fitSize("保存").x + fitSize("重载").x + 4.0f;
            // 内容区总宽 = Max.x - Min.x(非当前行剩余空间 avail)
            const float contentMin = ImGui::GetWindowContentRegionMin().x;
            const float contentMax = ImGui::GetWindowContentRegionMax().x;
            const float rightAlignX = (contentMax - contentMin) - rightGroupW;
            if (rightAlignX > ImGui::GetCursorPosX()) {
                ImGui::SameLine(rightAlignX); // 相对行首(内容区起点)偏移,推到右缘
            }
            if (ImGui::Button("保存", fitSize("保存"))) {
                ECS::SceneSerializer serializer;
                std::string savePath = "auto_save.json";
                if (serializer.SaveScene(savePath)) {
                    printf("场景自动保存成功：%s", savePath.c_str());
                } else {
                    printf("场景保存失败");
                }
            }
            ImGui::SameLine(0, 4);
            if (ImGui::Button("重载", fitSize("重载"))) {
                ECS::SceneSerializer serializer;
                std::string loadPath = "auto_save.json";
                if (serializer.LoadScene(loadPath)) {
                    printf("场景重载成功：%s", loadPath.c_str());
                } else {
                    printf("场景重载失败：%s", loadPath.c_str());
                }
            }
        }
        ImGui::PopStyleVar(); // FramePadding
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace Editor
