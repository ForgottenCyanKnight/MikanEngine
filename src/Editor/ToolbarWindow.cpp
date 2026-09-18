#include "Editor/ToolbarWindow.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "ECS/SceneECS.h"
#include "SceneSerializer.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"
#include "Core/Utf8Path.h"
#include "Core/TerrainPaintPersistence.h"
#include <filesystem>

namespace Editor {

// Game.dll 导出: 加载场景文件并按场景 "game" 键自动激活游戏模块
#ifdef _WIN32
extern "C" __declspec(dllimport) void MikanEngine_LoadSceneFile(const char* path);
#else
extern "C" void MikanEngine_LoadSceneFile(const char* path);
#endif

namespace {
// 重载确认弹窗的 ImGui 标识；弹窗在工具栏 child 之外提交，避免作用域嵌套。
constexpr const char* kReloadConfirmPopup = "重载场景##toolbar_reload_confirm";
} // namespace

ToolbarWindow& ToolbarWindow::GetInstance() {
    static ToolbarWindow instance;
    return instance;
}

std::string ToolbarWindow::GetActiveScenePath() const {
    return ProjectManager::GetInstance().GetActiveScenePath();
}

bool ToolbarWindow::SaveActiveScene() {
    const std::string path = GetActiveScenePath();
    if (path.empty()) {
        LOGE("[Toolbar] 保存失败：未选择项目，或 project.json 未声明 scene");
        return false;
    }

    // WriteFileAtomically 不会建目录；清单里的 scene 指向尚未存在的子目录时先补齐。
    std::error_code ec;
    const std::filesystem::path sceneFile = Utf8Path(path);
    if (sceneFile.has_parent_path()) {
        std::filesystem::create_directories(sceneFile.parent_path(), ec);
    }

    // 显式保存前先落盘地形笔刷产物（雕刻/涂色/涂草），产物路径随本次
    // SaveScene 一并写进场景 JSON；重载时渲染与物理碰撞优先走产物。
    {
        std::string paintError;
        TerrainPaintPersistence::SaveTerrainPaintData(path, &paintError);
        if (!paintError.empty()) {
            LOGE("[Toolbar] 地形笔刷产物保存失败: %s", paintError.c_str());
        }
    }

    ECS::SceneSerializer serializer;
    if (!serializer.SaveScene(path)) {
        LOGE("[Toolbar] 场景保存失败: %s", path.c_str());
        return false;
    }
    LOGI("[Toolbar] 场景已保存: %s", path.c_str());
    return true;
}

bool ToolbarWindow::ReloadActiveScene() {
    const std::string path = GetActiveScenePath();
    if (path.empty()) {
        LOGE("[Toolbar] 重载失败：未选择项目，或 project.json 未声明 scene");
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(Utf8Path(path), ec)) {
        LOGE("[Toolbar] 重载失败：场景文件不存在: %s", path.c_str());
        return false;
    }

    // 经引擎导出的加载入口走完整的场景切换流程(清理物理/瓦片 → 反序列化 →
    // 按 "game" 键激活玩法)。路径已是当前场景，导入结果不会改变保存目标。
    LOGI("[Toolbar] 重载场景: %s", path.c_str());
    MikanEngine_LoadSceneFile(path.c_str());
    return true;
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
            LOGI("运行游戏");
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
            LOGI("%s", m_isGamePaused ? "游戏暂停" : "继续游戏");
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
            LOGI("停止游戏");
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

        ImGui::SameLine(0, 20);

        // 网格按钮:开关 SceneView 网格(地面网格线 + 原点 X/Z/Y 坐标轴同属网格 pass)
        ImGui::PushStyleColor(ImGuiCol_Button, m_showGrid ? ImVec4(0.2f, 0.5f, 0.8f, 1.0f) : ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("网格", fitSize("网格"))) {
            m_showGrid = !m_showGrid;
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
            // 游玩态的场景是内存快照隔离出来的临时状态：此时保存会把运行时改动
            // 写进项目场景文件，重载则会打断运行，故两者在运行期间一并禁用。
            ImGui::BeginDisabled(m_isGameRunning);
            if (ImGui::Button("保存", fitSize("保存"))) {
                SaveActiveScene();
            }
            const bool hoverSave = m_isGameRunning &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            ImGui::SameLine(0, 4);
            if (ImGui::Button("重载", fitSize("重载"))) {
                m_confirmReload = true;
            }
            const bool hoverReload = m_isGameRunning &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            ImGui::EndDisabled();
            if (hoverSave || hoverReload) {
                ImGui::SetTooltip("游玩中不可用：请先停止游戏");
            }
        }
        ImGui::PopStyleVar(); // FramePadding
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    RenderReloadConfirmPopup();
}

void ToolbarWindow::RenderReloadConfirmPopup() {
    if (m_confirmReload) {
        m_confirmReload = false;
        ImGui::OpenPopup(kReloadConfirmPopup);
    }
    if (!ImGui::BeginPopupModal(kReloadConfirmPopup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextUnformatted("重载当前场景？");
    ImGui::Spacing();
    ImGui::TextDisabled("未保存的修改将丢失。");

    const std::string scenePath = GetActiveScenePath();
    if (!scenePath.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", scenePath.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("重载", ImVec2(120.0f, 0.0f))) {
        ReloadActiveScene();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace Editor
