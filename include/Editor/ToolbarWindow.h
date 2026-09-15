#pragma once

#include <cstdint>
#include <string>
#include "Editor/GizmoMode.h"

namespace Editor {

class ToolbarWindow {
public:
    static ToolbarWindow& GetInstance();

    bool IsShowGizmoAxis() const { return m_showGizmoAxis; }
    void ToggleGizmoAxis() { m_showGizmoAxis = !m_showGizmoAxis; }

    // SceneView 网格显示（地面网格线 + 原点 X/Z/Y 坐标轴，二者同属网格 pass）。
    // 每帧同步到 Core 的 g_ShowGrid，由 InfiniteGridRenderer 消费。
    bool IsShowGrid() const { return m_showGrid; }
    void SetShowGrid(bool show) { m_showGrid = show; }
    void ToggleGrid() { m_showGrid = !m_showGrid; }

    GizmoMode GetGizmoMode() const { return m_currentGizmoMode; }
    void SetGizmoMode(GizmoMode mode) { m_currentGizmoMode = mode; }

    bool IsGameRunning() const { return m_isGameRunning; }
    void SetGameRunning(bool running) { m_isGameRunning = running; }

    bool IsGamePaused() const { return m_isGamePaused; }
    void SetGamePaused(bool paused) { m_isGamePaused = paused; }

    void Render();

    // 工具栏「保存」：把当前场景写回它自己的场景文件（当前项目 project.json
    // 的 scene 字段，或用户经「文件 → 加载场景」导入的文件）。未选项目时失败。
    bool SaveActiveScene();
    // 工具栏「重载」：从当前场景文件重新加载，丢弃未保存的修改。
    bool ReloadActiveScene();

    // 当前场景文件路径（供界面提示）；无项目/无场景时为空。
    std::string GetActiveScenePath() const;

private:
    ToolbarWindow() = default;
    ~ToolbarWindow() = default;
    ToolbarWindow(const ToolbarWindow&) = delete;
    ToolbarWindow& operator=(const ToolbarWindow&) = delete;

    // 「重载」确认弹窗：重载会丢弃未保存改动，先确认再执行。
    void RenderReloadConfirmPopup();

    bool m_showGizmoAxis = true;
    bool m_showGrid = true;
    GizmoMode m_currentGizmoMode = GizmoMode::Translate;
    bool m_isGameRunning = false;
    bool m_isGamePaused = false;
    bool m_confirmReload = false; // 本帧点了「重载」，待弹确认框
};

} // namespace Editor