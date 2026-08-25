// EditorDllApi.cpp - Editor.dll entry points
// The runtime (Game.dll) loads this module at startup if present and calls
// these functions to enter editor mode. Everything editor-specific lives here:
// ImGui, docking windows, gizmos, asset browser, previews.
#include "EditorManager.h"
#include "Editor/HierarchyWindow.h"

#include "Editor/MRTDebugWindow.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/SceneViewWindow.h"
#include "Editor/GameViewWindow.h"
#include "Editor/PropertiesWindow.h"
#include "Editor/ControlPanelWindow.h"
#include "Editor/ProjectManagerWindow.h"
#include "Editor/UndoManager.h"
#include "Editor/ToolbarWindow.h"
#include "SceneSerializer.h"
#include "Core/ProjectManager.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "VulkanManager.h"
#include "Camera.h"
#include "ECS/SceneECS.h"
#include "RenderTarget.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include "ImGuizmo.h"
#include <glm/gtc/matrix_transform.hpp>

// Game.dll globals consumed by the editor
extern MIKAN_API RenderTarget g_SceneRenderTarget;
extern MIKAN_API RenderTarget g_GameRenderTarget;

// Game.dll 瀵煎嚭: 鐑噸杞藉綋鍓嶆父鎴忔彃浠?extern "C" __declspec(dllimport) bool MikanEngine_ReloadCurrentGame();

// ===== 缂栬緫鍣ㄤ晶: 缂栬瘧 games/ 鎻掍欢骞剁儹閲嶈浇褰撳墠娓告垙(椤婚潪杩愯鎬?=====
// MainMenuBar 鑿滃崟椤逛笌 F5 蹇嵎閿叡鐢ㄣ€傜紪璇戣剼鏈?tools/compile_games.ps1(VsDevCmd + cl)銆?void ReloadGamePluginAction() {
// Game.dll export: hot-reload the current game plugin (declaration)
extern "C" __declspec(dllimport) bool MikanEngine_ReloadCurrentGame();

// Last compile error log (shown in an editor popup when auto-recompile fails)
static std::string g_compileErrorLog;

const std::string& GetLastCompileErrorLog() { return g_compileErrorLog; }
void ClearCompileError() { g_compileErrorLog.clear(); }

// Editor-side: compile games/ plugins and hot-reload the current game (needs game stopped)
void ReloadGamePluginAction() {
    fprintf(stderr, "[Editor] F5 hot-reload triggered\n");
    if (Editor::ToolbarWindow::GetInstance().IsGameRunning()) {
        fprintf(stderr, "[Editor] Stop the game first, then reload plugin\n");
        return;
    }
    // 2026-08: use engine root (no hardcoded path); capture the log for the error popup.
    const std::string root = ProjectManager::GetInstance().GetEngineRoot();
    const std::string logPath = root + "out/build/games_compile.log";
    const std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + root + "tools/compile_games.ps1\" > \"" + logPath + "\" 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc == 0) {
        g_compileErrorLog.clear();
        if (MikanEngine_ReloadCurrentGame()) {
            fprintf(stderr, "[Editor] Game plugin hot-reloaded (engine not restarted)\n");
        } else {
            fprintf(stderr, "[Editor] Reload failed (active game is not a plugin)\n");
        }
    } else {
        // Read the compile log for the popup
        std::ifstream in(logPath);
        if (in.is_open()) {
            g_compileErrorLog.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (g_compileErrorLog.empty()) {
            g_compileErrorLog = "compile_games.ps1 failed (rc=" + std::to_string(rc) + ")";
        }
        fprintf(stderr, "[Editor] Plugin compile failed (code=%d), see %s\n", rc, logPath.c_str());
    }
}

// Game mode: render the game view as a borderless fullscreen overlay.
// The control panel is drawn afterwards so it stays on top.
static void RenderGameViewFullscreen()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowDockID(0); // detach from the docking layout
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("游戏视图 (游戏模式)", nullptr, flags))
    {
        VkDescriptorSet ds = Editor::GameViewWindow::GetInstance().GetDescriptorSet();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (ds != VK_NULL_HANDLE && avail.x > 1.0f && avail.y > 1.0f)
            ImGui::Image((ImTextureID)ds, avail);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

extern "C" {

__declspec(dllexport) bool MikanEditor_Attach(SDL_Window* window, int w, int h, float scale)
{
    EditorManager::GetInstance().InitImGui(window, w, h, scale);
    EditorManager::GetInstance().Init();
    // 视口面板采样"显示附件"（合成 subpass 输出）——编辑视口与游戏输出同一管线
    EditorManager::GetInstance().SetSceneViewDescriptorSet(g_SceneRenderTarget.GetDisplayDescriptorSet());
    EditorManager::GetInstance().SetGameViewDescriptorSet(g_GameRenderTarget.GetDisplayDescriptorSet());
    return true;
}

__declspec(dllexport) void MikanEditor_RenderFrame()
{
    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    ImGuizmo::BeginFrame();

    // ===== Undo/Redo: 娓告垙杩愯鎬?鎾斁)鍏抽棴鎾ら攢褰曞埗,鍋滄鍚庢仮澶?=====
    // 杩愯涓尅鏉?鐞冩瘡甯т綅绉绘槸娓告垙琛屼负,涓嶅簲璁颁负缂栬緫鍣ㄦ搷浣?涔熺渷鎺夊叏鍦烘櫙搴忓垪鍖栧紑閿€
    {
        const bool running = Editor::ToolbarWindow::GetInstance().IsGameRunning() &&
                             !Editor::ToolbarWindow::GetInstance().IsGamePaused();
        static bool s_wasRunning = false;
        if (running && !s_wasRunning) {
            Editor::UndoManager::GetInstance().SetRecordingEnabled(false);
        } else if (!running && s_wasRunning) {
            Editor::UndoManager::GetInstance().SetRecordingEnabled(true);
        }
        s_wasRunning = running;
    }
    // ===== Undo/Redo 甯ф娴?鑷姩璁板綍鍦烘櫙鍙樻洿鎾ら攢鐐?=====
    Editor::UndoManager::GetInstance().UpdateFrameDetection();

    // ===== F5: 閲嶆柊缂栬瘧骞剁儹閲嶈浇娓告垙鎻掍欢(涓嶉噸鍚紩鎿?椤婚潪杩愯鎬?=====
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        ReloadGamePluginAction();
    }

    // ===== 蹇嵎閿? Ctrl+Z 鎾ら攢 / Ctrl+Y 閲嶅仛 / Ctrl+S 淇濆瓨鍦烘櫙 =====
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                Editor::UndoManager::GetInstance().Undo();
            } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                Editor::UndoManager::GetInstance().Redo();
            } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                ECS::SceneSerializer serializer;
                std::string savePath = "auto_save.json";
                if (serializer.SaveScene(savePath)) {
                    printf("鍦烘櫙宸蹭繚瀛? %s\n", savePath.c_str());
                } else {
                    printf("鍦烘櫙淇濆瓨澶辫触\n");
                }
            }
        }
    }

    if (g_ProjectSelectionPending) {
        Editor::ProjectManagerWindow::GetInstance().SetVisible(true);
        // 椤圭洰绠＄悊鍣ㄥ惎鍔ㄩ〉(鏈寚瀹?--project):閫夋嫨椤圭洰鍓嶅彧鏄剧ず椤圭洰鍒楄〃,涓嶆覆鏌撶紪杈戝櫒绐楀彛/瑙嗗浘
        Editor::ProjectManagerWindow::GetInstance().Render();
    }
    else if (g_RunMode == RunMode::Game)
    {
        // Game mode: hide all editor windows, fullscreen the game view, keep the control panel
        RenderGameViewFullscreen();
        Editor::ControlPanelWindow::GetInstance().Render();
    }
    else
    {
        // Editor mode: full UI
        // Docking layout + modular editor windows
        EditorManager::GetInstance().RenderDockingLayout();
        Editor::HierarchyWindow::GetInstance().Render();
        Editor::MRTDebugWindow::GetInstance().Render();
        Editor::MaterialEditorWindow::GetInstance().Render();

        // Scene view (with gizmo) 鈥斺€?2D 娓告垙涓嶆樉绀?3D gizmo(2D 瀹炰綋鐢?GameView 鐨?2D gizmo 澶勭悊)
        ECS::Entity selectedEntity = ECS::SceneECS::GetInstance().GetSelectedEntity();
        if (selectedEntity != ECS::INVALID_ENTITY && g_ShowAxis && !g_SceneIs2D) {
            glm::mat4 view = g_Camera.GetViewMatrix();
            glm::mat4 proj = glm::perspective(glm::radians(EngineConfig::FOV),
                (float)g_MainWindowData.Width / (float)g_MainWindowData.Height,
                EngineConfig::NEAR_PLANE, EngineConfig::FAR_PLANE);
            Editor::SceneViewWindow::GetInstance().SetGizmoMode(static_cast<Editor::GizmoMode>(g_GizmoMode));
            Editor::SceneViewWindow::GetInstance().SetShowGizmoAxis(g_ShowAxis);
            Editor::SceneViewWindow::GetInstance().RenderWithGizmo(EditorManager::GetInstance().m_showSceneView, view, proj, selectedEntity);
        } else {
            Editor::SceneViewWindow::GetInstance().Render(EditorManager::GetInstance().m_showSceneView);
        }

        if (EditorManager::GetInstance().m_showGameView)
            Editor::GameViewWindow::GetInstance().Render(EditorManager::GetInstance().m_showGameView);

        EditorManager::GetInstance().RenderAssetsWindow();
        Editor::PropertiesWindow::GetInstance().Render();
        Editor::ControlPanelWindow::GetInstance().Render();
        // 编辑器运行中也可从“项目”菜单打开项目管理器，切换或导入项目。
        Editor::ProjectManagerWindow::GetInstance().Render();
    }

    // Virtual joystick overlay (drawn before ImGui::Render so the draw list is valid)
    g_InputController.RenderTouchControls();

    // Sync editor settings to Game.dll globals for the render pipeline
    g_ShowGrid = EditorManager::GetInstance().ShowGrid();
    // 仅当窗口可见且为激活标签页时才渲染对应视图（后台/未激活标签不渲染内容）
    g_ShowSceneView = EditorManager::GetInstance().m_showSceneView && Editor::SceneViewWindow::GetInstance().IsVisible();
    g_ShowGameView = EditorManager::GetInstance().m_showGameView && Editor::GameViewWindow::GetInstance().IsVisible();

    ImGui::Render();
}

__declspec(dllexport) void MikanEditor_Detach()
{
    EditorManager::GetInstance().Cleanup();
    EditorManager::GetInstance().ShutdownImGui();
}

__declspec(dllexport) void MikanEditor_SetSceneViewDescriptor(VkDescriptorSet ds)
{
    EditorManager::GetInstance().SetSceneViewDescriptorSet(ds);
}

__declspec(dllexport) void MikanEditor_SetGameViewDescriptor(VkDescriptorSet ds)
{
    EditorManager::GetInstance().SetGameViewDescriptorSet(ds);
}

__declspec(dllexport) bool MikanEditor_IsGameRunning()
{
    return EditorManager::GetInstance().m_isGameRunning;
}

__declspec(dllexport) bool MikanEditor_IsGamePaused()
{
    return EditorManager::GetInstance().m_isGamePaused;
}

} // extern "C"
