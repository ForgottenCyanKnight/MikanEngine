#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include "EngineGlobal.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "TexturePool.h"
#include "Editor/GizmoMode.h"
#include "Editor/SceneViewWindow.h"
#include "Editor/GameViewWindow.h"
#include "Editor/AssetsWindow.h"

class EditorManager {
public:
    static EditorManager& GetInstance();
    
    void Init();
    void InitImGui(SDL_Window* window, int width, int height, float main_scale);
    void ShutdownImGui();
    void Cleanup();
    
    void RenderDockingLayout();
    void RenderSceneView();
    void RenderSceneViewWithGizmo(const glm::mat4& view, const glm::mat4& proj, ECS::Entity selectedEntity, GizmoMode gizmoMode, bool showAxis);
    void RenderAssetsWindow();
    void RenderGameView();
    void RenderGizmo(const glm::mat4& view, const glm::mat4& proj, ECS::Entity selectedEntity, GizmoMode gizmoMode, bool showAxis);
    void RenderMainToolbar();
    
    void SetAssetsRootPath(const std::string& path);
    std::string GetSelectedAssetPath() const { return Editor::AssetsWindow::GetInstance().GetSelectedAssetPath(); }
    std::string GetCurrentDirectory() const { return Editor::AssetsWindow::GetInstance().GetCurrentDirectory(); }
    
    void SetSceneViewDescriptorSet(VkDescriptorSet descriptorSet) { Editor::SceneViewWindow::GetInstance().SetDescriptorSet(descriptorSet); }
    VkDescriptorSet GetSceneViewDescriptorSet() const { return Editor::SceneViewWindow::GetInstance().GetDescriptorSet(); }
    
    void SetGameViewDescriptorSet(VkDescriptorSet descriptorSet) { Editor::GameViewWindow::GetInstance().SetDescriptorSet(descriptorSet); }
    VkDescriptorSet GetGameViewDescriptorSet() const { return Editor::GameViewWindow::GetInstance().GetDescriptorSet(); }
    
    void GetSceneViewSize(float& width, float& height) const { width = Editor::SceneViewWindow::GetInstance().GetWidth(); height = Editor::SceneViewWindow::GetInstance().GetHeight(); }
    bool IsSceneViewSizeChanged() const { return Editor::SceneViewWindow::GetInstance().IsSizeChanged(); }
    void ResetSceneViewSizeChanged() { Editor::SceneViewWindow::GetInstance().ResetSizeChanged(); }
    
    TexturePool* GetTexturePool() const { return m_TexturePool.get(); }
    
    bool m_showSceneView = true;
    bool m_showGameView = true;
    bool m_isSceneViewVisible = false;
    bool m_isGameViewVisible = false;
    bool m_showAssetsWindow = true;
    bool m_showLogWindow = false;
    
    bool m_showGizmoAxis = true;
    GizmoMode m_currentGizmoMode = GizmoMode::Translate;
    bool m_isGameRunning = false;
    bool m_isGamePaused = false;
    
    bool SaveMaterialToFile(const std::string& filePath, const ECS::MaterialComponent& material);
    bool LoadMaterialFromFile(const std::string& filePath, ECS::MaterialComponent& material);
    void EditMaterial(const std::string& filePath);
    
    void UpdateAssetCache();

private:
    EditorManager();
    ~EditorManager() = default;
    
    EditorManager(const EditorManager&) = delete;
    EditorManager& operator=(const EditorManager&) = delete;
    
    std::unique_ptr<TexturePool> m_TexturePool;
};
