#pragma once

#include "ECS/Types.h"

#include <string>
#include <unordered_map>

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
    char m_searchBuffer[256]{};
    std::string m_searchText;
    mutable std::unordered_map<ECS::Entity, bool> m_searchSubtreeCache;

    void RenderHierarchyChildren(unsigned int parent, unsigned int selectedEntity, bool searchActive);
    void RenderVisibilityToggle(unsigned int entity);  // 行尾小眼睛（●可见/○隐藏）
    bool MatchesSearch(unsigned int entity) const;
    bool SubtreeMatchesSearch(unsigned int entity) const;
};

} // namespace Editor
