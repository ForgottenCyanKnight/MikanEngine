#pragma once

#include <string>

namespace Editor {

class DockingLayout {
public:
    static DockingLayout& GetInstance();
    
    void RenderDockingLayout();
    void RenderSceneView();
    void RenderGameView();
    void RenderMainToolbar();
    
    bool IsLayoutInitialized() const { return m_layoutInitialized; }
    
    bool m_showSceneView = true;
    bool m_showGameView = true;
    bool m_showAssetsWindow = true;
    bool m_showTilemapEditor = false;
    
    bool m_showGizmoAxis = true;
    bool m_isGameRunning = false;
    bool m_isGamePaused = false;

private:
    DockingLayout();
    ~DockingLayout() = default;
    
    DockingLayout(const DockingLayout&) = delete;
    DockingLayout& operator=(const DockingLayout&) = delete;
    
    bool m_layoutInitialized = false;
};

} // namespace Editor
