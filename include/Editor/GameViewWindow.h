#pragma once

#include <vulkan/vulkan.h>
#include <imgui/imgui.h>

namespace Editor {

class GameViewWindow {
public:
    static GameViewWindow& GetInstance();

    void SetDescriptorSet(VkDescriptorSet descriptorSet) { m_descriptorSet = descriptorSet; }
    VkDescriptorSet GetDescriptorSet() const { return m_descriptorSet; }

    bool IsVisible() const { return m_isVisible; }

    void Render(bool& showWindow);
    void DrawUIEntityGizmo(const ImVec2& contentSize);  // 2D UI 实体屏幕坐标 gizmo

private:
    GameViewWindow() = default;
    ~GameViewWindow() = default;
    GameViewWindow(const GameViewWindow&) = delete;
    GameViewWindow& operator=(const GameViewWindow&) = delete;

    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    bool m_isVisible = false;
};

} // namespace Editor