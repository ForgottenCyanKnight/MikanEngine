#include "Editor/MainMenuBar.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "Editor/HierarchyWindow.h"
#include "Editor/PropertiesWindow.h"
#include "Editor/ControlPanelWindow.h"
#include "Editor/MRTDebugWindow.h"
#include "Editor/ToolbarWindow.h"
#include "ECS/SceneECS.h"
#include "SceneSerializer.h"
#include "Core/ProjectManager.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <string>

namespace Editor {

// Game.dll 导出: 加载场景文件并按场景 "game" 键自动激活游戏模块
extern "C" __declspec(dllimport) void MikanEngine_LoadSceneFile(const char* path);

} // namespace Editor

// EditorDllApi.cpp 提供(全局命名空间,同 Editor.dll): 编译 games/ 插件并热重载当前游戏
void ReloadGamePluginAction();
const std::string& GetLastCompileErrorLog();
void ClearCompileError();

namespace Editor {

// ===== 自动检测游戏代码变化 → 重新编译热重载（Unity 式 Ctrl+S 工作流）=====
// 500ms 限频轮询当前项目 games/ 下源文件 LastWriteTime；变化即触发编译（复用 F5 动作）。
static void PollGameCodeChanges() {
    static auto lastPoll = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll < std::chrono::milliseconds(500)) return;
    lastPoll = now;

    if (ToolbarWindow::GetInstance().IsGameRunning()) return; // 运行态不自动重编译

    const std::string gamesDir = ProjectManager::GetInstance().GetProjectRoot() + "games/";
    std::error_code ec;
    if (!std::filesystem::is_directory(gamesDir, ec)) return; // 旧式/无项目：跳过

    static std::unordered_map<std::string, std::filesystem::file_time_type> baseline;
    bool changed = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(gamesDir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".hxx") continue;
        auto ft = entry.last_write_time();
        auto it = baseline.find(entry.path().string());
        if (it == baseline.end()) {
            baseline[entry.path().string()] = ft; // 首次扫描建立基线
        } else if (it->second != ft) {
            it->second = ft;
            changed = true;
        }
    }
    if (changed) {
        fprintf(stderr, "[Editor] Game code changed - auto recompiling...\n");
        ReloadGamePluginAction();
    }
}

// ===== 编译失败弹窗：显示 games_compile.log 内容 =====
static void RenderCompileErrorPopup() {
    const std::string& err = GetLastCompileErrorLog();
    if (err.empty()) return;
    if (ImGui::Begin("编译错误 (Compile Error)", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextWrapped("%s", err.c_str());
        ImGui::Spacing();
        if (ImGui::Button("关闭 (Close)")) {
            ClearCompileError();
        }
    }
    ImGui::End();
}

MainMenuBar& MainMenuBar::GetInstance() {
    static MainMenuBar instance;
    return instance;
}

void MainMenuBar::Render(bool& showSceneView, bool& showGameView, bool& showAssetsWindow, bool& showTilemapEditor, bool& layoutInitialized) {
    // 2026-08 Unity 式迭代：自动检测 games/ 源码变化 → 重编译热重载 + 编译错误弹窗
    PollGameCodeChanges();
    RenderCompileErrorPopup();

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("保存场景")) {
                ECS::SceneSerializer serializer;
                std::string filepath = serializer.SaveFileDialog();
                if (!filepath.empty()) {
                    if (serializer.SaveScene(filepath)) {
                        printf("场景保存成功: %s", filepath.c_str());
                    } else {
                        printf("场景保存失败");
                    }
                }
            }
            if (ImGui::MenuItem("加载场景")) {
                // 经 MikanEngine_LoadSceneFile 加载: 场景带 "game" 键时自动激活对应游戏
                ECS::SceneSerializer serializer;
                std::string filepath = serializer.OpenFileDialog();
                if (!filepath.empty()) {
                    MikanEngine_LoadSceneFile(filepath.c_str());
                }
            }
            if (ImGui::MenuItem("重新编译并重载游戏插件 (F5)")) {
                // 编译 games/ 插件 DLL 并热重载当前游戏(不重启引擎;须先停止游戏)
                ReloadGamePluginAction();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            ImGui::MenuItem("场景视图", nullptr, &showSceneView);
            ImGui::MenuItem("游戏视图", nullptr, &showGameView);
            
            bool hierarchyVisible = Editor::HierarchyWindow::GetInstance().IsVisible();
            if (ImGui::MenuItem("层级", nullptr, &hierarchyVisible)) {
                Editor::HierarchyWindow::GetInstance().SetVisible(hierarchyVisible);
            }
            
            ImGui::MenuItem("资源", nullptr, &showAssetsWindow);
            
            bool tilemapVisible = showTilemapEditor;
            if (ImGui::MenuItem("瓦片编辑器", nullptr, &tilemapVisible)) {
                showTilemapEditor = tilemapVisible;
            }
            
            bool propertiesVisible = Editor::PropertiesWindow::GetInstance().IsVisible();
            if (ImGui::MenuItem("属性", nullptr, &propertiesVisible)) {
                Editor::PropertiesWindow::GetInstance().SetVisible(propertiesVisible);
            }
            
            bool controlPanelVisible = Editor::ControlPanelWindow::GetInstance().IsVisible();
            if (ImGui::MenuItem("控制面板", nullptr, &controlPanelVisible)) {
                Editor::ControlPanelWindow::GetInstance().SetVisible(controlPanelVisible);
            }
            
            bool mrtVisible = Editor::MRTDebugWindow::GetInstance().IsVisible();
            if (ImGui::MenuItem("MRT 调试", nullptr, &mrtVisible)) {
                Editor::MRTDebugWindow::GetInstance().SetVisible(mrtVisible);
            }
            
            ImGui::Separator();
            
            if (ImGui::MenuItem("重置布局")) {
                ImGui::ClearIniSettings();
                layoutInitialized = false;
            }
            
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

} // namespace Editor