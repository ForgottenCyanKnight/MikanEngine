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
#include "Core/Utf8Path.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
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
    // 使用引擎工具脚本，但把当前项目传入，避免重新扫描仓库内其他 games/。
    const std::string root = ProjectManager::GetInstance().GetEngineRoot();
    const std::string projectRoot = ProjectManager::GetInstance().GetProjectRoot();
    if (projectRoot.empty()) {
        g_compileErrorLog = "No project selected; gameplay compilation skipped";
        fprintf(stderr, "[Editor] Cannot compile gameplay without a selected project\n");
        return;
    }
    const std::string logPath = root + "out/build/games_compile.log";
    const std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" +
        root + "tools/compile_games.ps1\" -ProjectPath \"" + projectRoot +
        "\" > \"" + logPath + "\" 2>&1";
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

// Editor-side: assemble a standalone game package for the selected project.
static std::string g_publishMessage;

const std::string& GetLastPublishMessage() { return g_publishMessage; }
void ClearPublishMessage() { g_publishMessage.clear(); }

#ifdef _WIN32
namespace {

std::wstring QuoteProcessArgument(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

bool RunPublishScript(const std::string& engineRoot,
                      const std::string& projectRoot,
                      const std::string& outputDirectory,
                      const std::string& logPath,
                      DWORD& exitCode,
                      std::string& errorMessage) {
    const std::wstring engineRootWide = mikanpath::Utf8ToWide(engineRoot);
    const std::wstring scriptWide = mikanpath::Utf8ToWide(engineRoot + "tools/publish.ps1");
    const std::wstring projectWide = mikanpath::Utf8ToWide(projectRoot);
    const std::wstring outputWide = mikanpath::Utf8ToWide(outputDirectory);
    const std::wstring logWide = mikanpath::Utf8ToWide(logPath);

    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;
    HANDLE logFile = CreateFileW(logWide.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &securityAttributes, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile == INVALID_HANDLE_VALUE) {
        errorMessage = "无法创建发布日志文件: " + logPath;
        return false;
    }

    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &securityAttributes, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        CloseHandle(logFile);
        errorMessage = "无法初始化发布进程输入句柄";
        return false;
    }

    std::wstring command = L"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File " +
        QuoteProcessArgument(scriptWide) + L" -ProjectPath " +
        QuoteProcessArgument(projectWide) + L" -OutDir " +
        QuoteProcessArgument(outputWide);
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = nullInput;
    startupInfo.hStdOutput = logFile;
    startupInfo.hStdError = logFile;

    PROCESS_INFORMATION processInfo{};
    const BOOL started = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        engineRootWide.c_str(),
        &startupInfo,
        &processInfo);
    const DWORD createProcessError = started ? ERROR_SUCCESS : GetLastError();
    CloseHandle(nullInput);
    CloseHandle(logFile);

    if (!started) {
        errorMessage = "无法启动发布脚本（Win32 error=" +
            std::to_string(createProcessError) + ")";
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    if (!GetExitCodeProcess(processInfo.hProcess, &exitCode)) {
        exitCode = 1;
        errorMessage = "无法读取发布脚本退出码";
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return errorMessage.empty();
}

} // namespace
#endif

void PublishProjectAction(const std::string& outputDirectory) {
    g_publishMessage.clear();
    if (outputDirectory.empty()) return;

    const std::string projectRoot = ProjectManager::GetInstance().GetProjectRoot();
    if (projectRoot.empty() || !ProjectManager::GetInstance().HasManifest()) {
        g_publishMessage = "发布失败：当前没有打开项目清单（project.json）。";
        return;
    }

    const std::string engineRoot = ProjectManager::GetInstance().GetEngineRoot();
    if (engineRoot.empty()) {
        g_publishMessage = "发布失败：无法定位引擎根目录。";
        return;
    }

    const std::string logPath = engineRoot + "out/build/publish.log";
#ifdef _WIN32
    DWORD exitCode = 1;
    std::string launchError;
    if (!RunPublishScript(engineRoot, projectRoot, outputDirectory, logPath,
                          exitCode, launchError)) {
        g_publishMessage = "发布失败：" + launchError + "\n日志：" + logPath;
        return;
    }
    if (exitCode == 0) {
        g_publishMessage = "发布完成。\n输出目录：" + outputDirectory +
                           "\nEditor.dll 未包含在发布包中。";
        fprintf(stderr, "[Editor] Project package created: %s\n", outputDirectory.c_str());
    } else {
        g_publishMessage = "发布失败（退出码 " + std::to_string(exitCode) +
                           "）。\n日志：" + logPath;
        fprintf(stderr, "[Editor] Project package failed (exit=%lu), see %s\n",
                static_cast<unsigned long>(exitCode), logPath.c_str());
    }
#else
    g_publishMessage = "发布失败：当前平台暂未实现系统发布进程调用。";
#endif
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
    Editor::UndoManager::GetInstance().UpdateFrameDetection();

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        ReloadGamePluginAction();
    }

    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                Editor::UndoManager::GetInstance().Undo();
            } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                Editor::UndoManager::GetInstance().Redo();
            } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                ECS::SceneSerializer serializer;
                const std::string assetsRoot = ProjectManager::GetInstance().GetAssetsDir();
                const std::string savePath = assetsRoot.empty()
                    ? std::string()
                    : assetsRoot + "auto_save.json";
                if (!savePath.empty() && serializer.SaveScene(savePath)) {
                    printf("鍦烘櫙宸蹭繚瀛? %s\n", savePath.c_str());
                } else {
                    printf("鍦烘櫙淇濆瓨澶辫触\n");
                }
            }
        }
    }

    if (g_ProjectSelectionPending) {
        Editor::ProjectManagerWindow::GetInstance().SetVisible(true);
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
        if (!g_SceneIs2D) {
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
