#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <imgui/imgui.h>
#include "ECS/Types.h"
#include "ToolbarWindow.h"
#include "Editor/GizmoMode.h"

namespace Editor {

class SceneViewWindow {
public:
    static SceneViewWindow& GetInstance();

    void SetDescriptorSet(VkDescriptorSet descriptorSet) { m_descriptorSet = descriptorSet; }
    VkDescriptorSet GetDescriptorSet() const { return m_descriptorSet; }

    bool IsVisible() const { return m_isVisible; }

    float GetWidth() const { return m_width; }
    float GetHeight() const { return m_height; }
    bool IsSizeChanged() const { return m_sizeChanged; }
    void ResetSizeChanged() { m_sizeChanged = false; }

    bool ShowGrid() const { return m_showGrid; }
    void SetShowGrid(bool show) { m_showGrid = show; }

    bool ShowGizmoAxis() const { return m_showGizmoAxis; }
    void SetShowGizmoAxis(bool show) { m_showGizmoAxis = show; }

    GizmoMode GetGizmoMode() const { return m_currentGizmoMode; }
    void SetGizmoMode(GizmoMode mode) { m_currentGizmoMode = mode; }

    void Render(bool& showWindow);
    void RenderWithGizmo(bool& showWindow, const glm::mat4& view, const glm::mat4& proj, ECS::Entity selectedEntity);

private:
    SceneViewWindow() = default;
    ~SceneViewWindow() = default;
    SceneViewWindow(const SceneViewWindow&) = delete;
    SceneViewWindow& operator=(const SceneViewWindow&) = delete;

    void RenderDragDropTarget(const ImVec2& windowPos);

    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    bool m_isVisible = false;
    float m_width = 0.0f;
    float m_height = 0.0f;
    float m_lastWidth = 0.0f;
    float m_lastHeight = 0.0f;
    bool m_sizeChanged = false;
    bool m_showGrid = false;
    bool m_showGizmoAxis = true;
    GizmoMode m_currentGizmoMode = GizmoMode::Translate;
};

} // namespace Editor