#pragma once

namespace Editor {

class MRTDebugWindow {
public:
    static MRTDebugWindow& GetInstance();

    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }

    void Render();

private:
    MRTDebugWindow() = default;
    ~MRTDebugWindow() = default;

    MRTDebugWindow(const MRTDebugWindow&) = delete;
    MRTDebugWindow& operator=(const MRTDebugWindow&) = delete;

    bool m_visible = false;
};

} // namespace Editor