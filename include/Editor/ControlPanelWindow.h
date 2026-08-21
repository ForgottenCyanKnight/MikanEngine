#pragma once

#include <string>

namespace Editor {

class ControlPanelWindow {
public:
    static ControlPanelWindow& GetInstance();
    
    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }
    
    void Render();
    
private:
    ControlPanelWindow() = default;
    ~ControlPanelWindow() = default;
    
    ControlPanelWindow(const ControlPanelWindow&) = delete;
    ControlPanelWindow& operator=(const ControlPanelWindow&) = delete;
    
    bool m_visible = true;
};

} // namespace Editor