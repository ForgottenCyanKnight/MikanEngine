#pragma once

#include <cstdint>
#include "Editor/GizmoMode.h"

namespace Editor {

class ToolbarWindow {
public:
    static ToolbarWindow& GetInstance();

    bool IsShowGizmoAxis() const { return m_showGizmoAxis; }
    void ToggleGizmoAxis() { m_showGizmoAxis = !m_showGizmoAxis; }

    GizmoMode GetGizmoMode() const { return m_currentGizmoMode; }
    void SetGizmoMode(GizmoMode mode) { m_currentGizmoMode = mode; }

    bool IsGameRunning() const { return m_isGameRunning; }
    void SetGameRunning(bool running) { m_isGameRunning = running; }

    bool IsGamePaused() const { return m_isGamePaused; }
    void SetGamePaused(bool paused) { m_isGamePaused = paused; }

    void Render();

private:
    ToolbarWindow() = default;
    ~ToolbarWindow() = default;
    ToolbarWindow(const ToolbarWindow&) = delete;
    ToolbarWindow& operator=(const ToolbarWindow&) = delete;

    bool m_showGizmoAxis = true;
    GizmoMode m_currentGizmoMode = GizmoMode::Translate;
    bool m_isGameRunning = false;
    bool m_isGamePaused = false;
};

} // namespace Editor