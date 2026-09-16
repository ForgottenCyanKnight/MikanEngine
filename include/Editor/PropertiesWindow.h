#pragma once

#include <string>

namespace Editor {

class PropertiesWindow {
public:
    static PropertiesWindow& GetInstance();

    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }
    void Render();

private:
    PropertiesWindow() = default;
    ~PropertiesWindow() = default;
    PropertiesWindow(const PropertiesWindow&) = delete;
    PropertiesWindow& operator=(const PropertiesWindow&) = delete;

    bool m_visible = true;
    // 待确认覆盖的预制体路径；空 = 无待确认保存
    std::string m_pendingPrefabSavePath;
};

} // namespace Editor