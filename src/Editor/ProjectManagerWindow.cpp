// ProjectManagerWindow.cpp - 项目管理器启动页(Godot 风格)
// 左右分栏:左侧菜单(第一项=项目列表,预留版本管理等),选中切换右侧内容。
// 打开项目调用 Game.dll 导出的 MikanEngine_OpenProject → 切换项目根 + 加载场景。
#include "Editor/ProjectManagerWindow.h"
#include "Editor/AssetsWindow.h"
#include "imgui/imgui.h"
#include "Core/ProjectManager.h"
#include "SceneSerializer.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <ctime>
#include <algorithm>

namespace Editor {

// Game.dll 导出(由 Editor.dll 链接调用)
extern "C" __declspec(dllimport) void MikanEngine_OpenProject(const char* dir);
extern "C" __declspec(dllimport) void MikanEngine_LoadSceneFile(const char* path);

ProjectManagerWindow& ProjectManagerWindow::GetInstance() {
    static ProjectManagerWindow instance;
    return instance;
}

static std::string ProjectsFilePath() {
    std::string root = ProjectManager::GetInstance().GetEngineRoot();
    return root.empty() ? std::string("projects.json") : (root + "projects.json");
}

// 项目判定：含 project.json（项目化，2026-08：场景=项目工作目录配置，资源区=项目根）或 assets/（旧式）
static bool DirectoryHasAssets(const std::string& dir) {
    std::filesystem::path p(dir);
    if (p.filename().string().empty()) p = p.parent_path();
    return std::filesystem::exists(p / "project.json") || std::filesystem::exists(p / "assets");
}

void ProjectManagerWindow::LoadProjects() {
    m_projects.clear();

    // 引擎根(单项目布局)总是作为第一个候选
    std::string engineRoot = ProjectManager::GetInstance().GetEngineRoot();
    if (!engineRoot.empty() && DirectoryHasAssets(engineRoot)) {
        ProjectEntry e;
        e.name = "Default Project";
        e.path = engineRoot;
        m_projects.push_back(e);
    }

    std::string path = ProjectsFilePath();
    std::ifstream in(path);
    if (!in.is_open()) {
        // 无注册表:用引擎根建一个,下次保存
        return;
    }
    try {
        nlohmann::json j;
        in >> j;
        for (const auto& item : j.value("projects", nlohmann::json::array())) {
            ProjectEntry e;
            e.name = item.value("name", "");
            // 2026-08 相对路径支持：存相对（引擎根内），读时解析为绝对，发布目录移动仍有效
            e.path = ProjectManager::GetInstance().ResolveProjectPath(item.value("path", ""));
            e.lastOpened = item.value("lastOpened", (long long)0);
            if (!e.name.empty() && !e.path.empty() && DirectoryHasAssets(e.path))
                m_projects.push_back(e);
        }
    } catch (const std::exception& ex) {
        std::cerr << "[ProjectManagerWindow] Failed to parse " << path << ": " << ex.what() << std::endl;
    }

    // 去重(引擎根已加过的不重复加)
    std::sort(m_projects.begin(), m_projects.end(),
        [](const ProjectEntry& a, const ProjectEntry& b) { return a.lastOpened > b.lastOpened; });
}

void ProjectManagerWindow::SaveProjects() {
    // 引擎根默认项目不写回(始终存在);写注册表里其他项目
    std::string engineRoot = ProjectManager::GetInstance().GetEngineRoot();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : m_projects) {
        if (!engineRoot.empty() && e.path == engineRoot) continue;
        nlohmann::json item;
        item["name"] = e.name;
        item["path"] = ProjectManager::GetInstance().ToProjectRelativePath(e.path);
        item["lastOpened"] = e.lastOpened;
        arr.push_back(item);
    }
    nlohmann::json j;
    j["projects"] = arr;

    std::string path = ProjectsFilePath();
    std::ofstream out(path);
    if (out.is_open()) {
        out << j.dump(2);
        std::cout << "[ProjectManagerWindow] Saved project list: " << path << std::endl;
    } else {
        std::cerr << "[ProjectManagerWindow] Failed to save project list: " << path << std::endl;
    }
}

void ProjectManagerWindow::RefreshProjectList() {
    LoadProjects();
}

void ProjectManagerWindow::OpenProject(const std::string& path) {
    for (auto& e : m_projects) {
        if (e.path == path) e.lastOpened = std::time(nullptr);
    }
    SaveProjects();
    m_visible = false;
    MikanEngine_OpenProject(path.c_str());
    // 2026-08 项目化：资产窗口根路径跟随当前项目资源区（SetProjectRoot 已在 OpenProject 内更新 GetAssetsDir）
    Editor::AssetsWindow::GetInstance().SetAssetsRootPath(ProjectManager::GetInstance().GetAssetsDir());
}

// 右侧内容:项目列表(表格 + 打开/移除 + 新建)
void ProjectManagerWindow::RenderProjectListTab() {
    if (ImGui::BeginTable("ProjectTable", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("项目", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("路径", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        for (auto& e : m_projects) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", e.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", e.path.c_str());
            ImGui::TableSetColumnIndex(2);
            std::string openLabel = "打开##" + e.path;
            if (ImGui::Button(openLabel.c_str(), ImVec2(60, 0))) {
                OpenProject(e.path);
            }
            ImGui::SameLine();
            std::string delLabel = "移除##" + e.path;
            bool isDefault = !ProjectManager::GetInstance().GetEngineRoot().empty() &&
                             e.path == ProjectManager::GetInstance().GetEngineRoot();
            if (!isDefault) {
                if (ImGui::Button(delLabel.c_str(), ImVec2(50, 0))) {
                    m_projects.erase(std::remove_if(m_projects.begin(), m_projects.end(),
                        [&](const ProjectEntry& x) { return x.path == e.path; }), m_projects.end());
                    SaveProjects();
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // 手动导入场景文件(任意 .json,加载后按场景 "game" 键自动激活游戏)
    if (ImGui::Button("导入场景文件…", ImVec2(160, 0))) {
        ECS::SceneSerializer s;
        std::string path = s.OpenFileDialog();
        if (!path.empty()) {
            MikanEngine_LoadSceneFile(path.c_str());
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(选择 snake.json / breakout.json 等)");

    ImGui::Spacing();

    // 新建项目
    if (!m_showNewDialog) {
        if (ImGui::Button("新建项目", ImVec2(120, 0))) {
            OpenNewDialog();
        }
    } else {
        ImGui::Text("新建项目");
        ImGui::InputText("项目名称", m_newName, ProjectManagerWindow::kNewNameSize);
        ImGui::InputText("项目路径", m_newPath, ProjectManagerWindow::kNewPathSize);
        if (m_errorMsg[0]) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_errorMsg);
        }
        ImGui::Spacing();
        if (ImGui::Button("创建并打开", ImVec2(120, 0))) {
            std::string name = m_newName;
            std::string path = m_newPath;
            if (name.empty()) {
                SetNewError("项目名称不能为空");
            } else if (path.empty()) {
                SetNewError("项目路径不能为空");
            } else {
                try {
                    std::filesystem::path dir(path);
                    dir /= name;
                    std::filesystem::create_directories(dir / "assets");
                    // 空场景模板
                    std::ofstream scene(dir / "assets" / "sence.json");
                    if (scene.is_open()) scene << "{\n  \"entities\": []\n}\n";

                    // 注册并打开
                    ProjectEntry e;
                    e.name = name;
                    e.path = dir.string();
                    e.lastOpened = std::time(nullptr);
                    m_projects.push_back(e);
                    SaveProjects();
                    OpenProject(e.path);
                } catch (const std::exception& ex) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "创建失败: %s", ex.what());
                    SetNewError(msg);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(80, 0))) {
            CloseNewDialog();
        }
    }
}

void ProjectManagerWindow::Render() {
    if (!m_visible) return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
    ImGui::Begin("项目管理器", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.9f, 1.0f), "MikanEngine - 项目管理器");
    ImGui::TextDisabled("选择要打开的项目,或新建一个项目");
    ImGui::Separator();
    ImGui::Spacing();

    // ===== 左右分栏:左侧菜单,右侧选中内容 =====
    const float leftW = 200.0f;

    ImGui::BeginChild("PM_Left", ImVec2(leftW, 0), true);
    ImGui::TextDisabled("菜单");
    ImGui::Separator();
    if (ImGui::Selectable("项目列表", m_selectedTab == 0)) m_selectedTab = 0;
    // 预留菜单项(版本管理 / 模板 / 设置):未实现,置灰占位
    ImGui::BeginDisabled();
    ImGui::Selectable("版本管理", false);
    ImGui::Selectable("模板", false);
    ImGui::Selectable("设置", false);
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("PM_Right", ImVec2(0, 0), false);
    if (m_selectedTab == 0) {
        if (!m_loadedOnce) {
            LoadProjects();
            m_loadedOnce = true;
        }
        RenderProjectListTab();
    } else {
        ImGui::TextDisabled("该功能即将推出");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace Editor
