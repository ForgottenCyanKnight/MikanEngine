#pragma once

#include <string>

namespace Editor {

class HierarchyWindow {
public:
    static HierarchyWindow& GetInstance();

    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }
    void Render();

private:
    HierarchyWindow() = default;
    ~HierarchyWindow() = default;
    HierarchyWindow(const HierarchyWindow&) = delete;
    HierarchyWindow& operator=(const HierarchyWindow&) = delete;

    bool m_visible = true;

    void RenderHierarchyChildren(unsigned int parent, unsigned int selectedEntity);
    void RenderVisibilityToggle(unsigned int entity);  // 行尾小眼睛（●可见/○隐藏）
};

} // namespace Editor