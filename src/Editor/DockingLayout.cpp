#include "Editor/DockingLayout.h"
#include "Editor/MainMenuBar.h"
#include "Editor/ToolbarWindow.h"
#include "Editor/SceneViewWindow.h"
#include "Editor/GameViewWindow.h"
#include "Editor/TilemapEditorWindow.h"
#include "imgui.h"
#include <algorithm>
#include "imgui_internal.h"

namespace Editor {

DockingLayout& DockingLayout::GetInstance() {
    static DockingLayout instance;
    return instance;
}

DockingLayout::DockingLayout() {
}

void DockingLayout::RenderDockingLayout() {
    ImGuiIO& io = ImGui::GetIO();
    
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
    window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    
    ImGui::Begin("DockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);
    
    MainMenuBar::GetInstance().Render(m_showSceneView, m_showGameView, m_showAssetsWindow, m_showTilemapEditor, m_layoutInitialized);
    
    ToolbarWindow::GetInstance().Render();

    // 瓦片编辑器(浮动窗口, 视图菜单开关)
    if (m_showTilemapEditor) {
        Editor::TilemapEditorWindow::GetInstance().Render(m_showTilemapEditor);
    }

    // 工具栏高度缩小后,吃掉其与 DockSpace 之间的 ItemSpacing 空隙,
    // 让停靠窗口(场景视图等)向上填充,利用被释放的垂直空间。
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);

    // 同步运行状态:工具栏"运行/暂停"按钮修改的是 ToolbarWindow 自身成员,
    // 而引擎侧(EngineMain)经由 EditorManager ← DockingLayout 读取运行状态。
    // 此处每帧桥接,否则按钮不生效 → 编辑器模式下物理/游戏逻辑永远不会运行。
    m_isGameRunning = ToolbarWindow::GetInstance().IsGameRunning();
    m_isGamePaused = ToolbarWindow::GetInstance().IsGamePaused();

    ImGui::BeginChild("DockSpaceArea", ImVec2(0, 0), false, ImGuiWindowFlags_None);
    ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    
    if (!m_layoutInitialized) {
        m_layoutInitialized = true;
        
        ImGuiDockNode* root_node = ImGui::DockBuilderGetNode(dockspace_id);
        bool hasExistingLayout = (root_node != nullptr && root_node->ChildNodes[0] != nullptr);
        
        if (!hasExistingLayout) {
            ImGuiID dockMain = dockspace_id;

            // Adaptive split ratios: side panels keep a reasonable pixel width and
            // the bottom log a reasonable height on any screen size / aspect ratio.
            ImVec2 workSize = viewport->WorkSize;
            float workW = workSize.x > 1.0f ? workSize.x : 1280.0f;
            float workH = workSize.y > 1.0f ? workSize.y : 720.0f;
            float leftRatio   = std::clamp(250.0f / workW, 0.14f, 0.24f);
            float rightRatio  = std::clamp(320.0f / workW, 0.18f, 0.30f);
            // 底部面板（资源窗口）默认高度比例略微调高，让面板更高、上沿上移一点点（仍停靠在底部）。
            float bottomRatio = std::clamp(300.0f / workH, 0.22f, 0.36f);

            ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, bottomRatio, nullptr, &dockMain);
            ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, leftRatio, nullptr, &dockMain);
            ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, rightRatio, nullptr, &dockMain);
            ImGuiID dockCenter = dockMain;

            // 中央区域: 场景视图 / 游戏视图 / 蓝图编辑器 同 dock 标签页共存(游戏视图默认折叠),
            // 场景视图最后 dock → 默认前台激活的是编辑器(场景视图)。
            ImGui::DockBuilderDockWindow("层级", dockLeft);
            ImGui::DockBuilderDockWindow("游戏视图", dockCenter);
            ImGui::DockBuilderDockWindow("蓝图编辑器", dockCenter);
            ImGui::DockBuilderDockWindow("场景视图", dockCenter);
            ImGui::DockBuilderDockWindow("属性", dockRight);
            ImGui::DockBuilderDockWindow("控制面板", dockRight);
            ImGui::DockBuilderDockWindow("资源", dockBottom);
            
            ImGui::DockBuilderFinish(dockspace_id);
        }
    }
    ImGui::EndChild();
    
    ImGui::End();
}

void DockingLayout::RenderSceneView() {
    SceneViewWindow::GetInstance().Render(m_showSceneView);
}

void DockingLayout::RenderGameView() {
    if (!m_showGameView) {
        return;
    }
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("游戏视图", &m_showGameView);
    
    bool isCollapsed = ImGui::IsWindowCollapsed();
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    float titleBarHeight = ImGui::GetFrameHeight();
    float windowHeight = ImGui::GetWindowHeight();
    bool hasVisibleContent = (windowHeight > titleBarHeight + 10.0f) && (contentSize.x > 1.0f && contentSize.y > 1.0f);
    
    ImGuiWindow* window = ImGui::FindWindowByName("游戏视图");
    bool isActiveTab = window ? (window->Flags & ImGuiWindowFlags_DockNodeHost) == 0 : true;
    if (window && window->DockNode) {
        isActiveTab = (window->DockNode->VisibleWindow == window);
    }
    
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    bool isOnScreen = (windowPos.x < viewport->WorkPos.x + viewport->WorkSize.x &&
                       windowPos.x + windowSize.x > viewport->WorkPos.x &&
                       windowPos.y < viewport->WorkPos.y + viewport->WorkSize.y &&
                       windowPos.y + windowSize.y > viewport->WorkPos.y);
    
    if (SceneViewWindow::GetInstance().GetDescriptorSet() != VK_NULL_HANDLE) {
        ImVec2 imageSize(contentSize.x, contentSize.y);
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);
        
        ImGui::Image((ImTextureID)SceneViewWindow::GetInstance().GetDescriptorSet(), imageSize, uv0, uv1);
    } else {
        ImVec2 contentAvail = ImGui::GetContentRegionAvail();
        ImVec2 textPos = ImVec2(
            ImGui::GetWindowPos().x + contentAvail.x * 0.5f - 50,
            ImGui::GetWindowPos().y + contentAvail.y * 0.5f
        );
        ImGui::SetCursorScreenPos(textPos);
        ImGui::Text("游戏视图");
        
        ImGui::SetCursorScreenPos(ImVec2(
            ImGui::GetWindowPos().x + contentAvail.x * 0.5f - 80,
            textPos.y + 20
        ));
        ImGui::TextDisabled("(游戏运行画面将显示在这里)");
    }
    
    ImGui::End();
    ImGui::PopStyleVar();
}

void DockingLayout::RenderMainToolbar() {
}

} // namespace Editor
