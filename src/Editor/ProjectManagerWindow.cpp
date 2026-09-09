// ProjectManagerWindow.cpp - 引擎项目管理器启动页
// 左右分栏:左侧项目导航,右侧项目列表内容。
// 打开项目调用 Game.dll 导出的 MikanEngine_OpenProject → 切换项目根 + 加载场景。
#include "Editor/ProjectManagerWindow.h"
#include "Core/Utf8Path.h"
#include "Editor/AssetsWindow.h"
#include "imgui/imgui.h"
#include "Core/ProjectManager.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace Editor {

namespace {
constexpr float kProjectManagerUiScale = 1.35f;
}

// Game.dll 导出(由 Editor.dll 链接调用)
extern "C" __declspec(dllimport) bool MikanEngine_OpenProject(const char* dir);

ProjectManagerWindow& ProjectManagerWindow::GetInstance() {
    static ProjectManagerWindow instance;
    return instance;
}

static std::string ProjectsFilePath() {
    std::string root = ProjectManager::GetInstance().GetEngineRoot();
    return root.empty() ? std::string("projects.json") : (root + "projects.json");
}

// 项目判定：必须包含 project.json；根目录 assets/ 不再是项目入口。
static bool DirectoryHasAssets(const std::string& dir) {
    std::filesystem::path p = Utf8Path(dir);
    if (p.filename().empty()) p = p.parent_path();
    return std::filesystem::exists(p / "project.json");
}

static std::string CanonicalProjectKey(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(Utf8Path(path), ec);
    if (ec) return path;
    std::string result = GenericUtf8String(canonical);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

static std::string OpenProjectManifestDialog() {
#ifdef _WIN32
    wchar_t buffer[32768] = {};
    const wchar_t filter[] = L"Mikan Project (project.json)\0project.json\0\0";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        return Utf8String(std::filesystem::path(buffer));
    }
#endif
    return {};
}

static std::string OpenProjectParentDirectoryDialog() {
#ifdef _WIN32
    // Use the native folder picker so users do not have to type a long or
    // non-ASCII parent path manually. Keep the input field as a fallback for
    // paths that are not exposed by the shell dialog.
    const HRESULT initResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) return {};
    const bool shouldUninitialize = SUCCEEDED(initResult);

    IFileDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(result)) {
        DWORD options = 0;
        result = dialog->GetOptions(&options);
        if (SUCCEEDED(result)) {
            result = dialog->SetOptions(
                options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        if (SUCCEEDED(result)) result = dialog->SetTitle(L"选择新项目的父目录");
        if (SUCCEEDED(result)) result = dialog->Show(GetActiveWindow());
    }

    std::string selectedPath;
    if (SUCCEEDED(result) && dialog) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR displayPath = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &displayPath)) &&
                displayPath) {
                selectedPath = Utf8String(std::filesystem::path(displayPath));
                CoTaskMemFree(displayPath);
            }
            item->Release();
        }
    }
    if (dialog) dialog->Release();
    if (shouldUninitialize) CoUninitialize();
    return selectedPath;
#else
    return {};
#endif
}

void ProjectManagerWindow::LoadProjects() {
    m_projects.clear();
    std::vector<std::string> knownPaths;
    const auto addProject = [&](ProjectEntry entry) {
        if (entry.name.empty() || entry.path.empty() ||
            !DirectoryHasAssets(entry.path)) {
            return;
        }
        const std::string key = CanonicalProjectKey(entry.path);
        if (std::find(knownPaths.begin(), knownPaths.end(), key) != knownPaths.end()) {
            return;
        }
        knownPaths.push_back(key);
        m_projects.push_back(std::move(entry));
    };

    std::string path = ProjectsFilePath();
    std::ifstream in(Utf8Path(path));
    if (!in.is_open()) {
        // 无注册表时保持空列表，由用户导入或新建项目。
        return;
    }
    try {
        nlohmann::json j;
        in >> j;
        for (const auto& item : j.value("projects", nlohmann::json::array())) {
            ProjectEntry e;
            e.name = item.value("name", "");
            e.path = ProjectManager::GetInstance().ResolveProjectPath(item.value("path", ""));
            e.lastOpened = item.value("lastOpened", (long long)0);
            addProject(std::move(e));
        }
    } catch (const std::exception& ex) {
        std::cerr << "[ProjectManagerWindow] Failed to parse " << path << ": " << ex.what() << std::endl;
    }

    // 最近打开的项目排在前面。
    std::sort(m_projects.begin(), m_projects.end(),
        [](const ProjectEntry& a, const ProjectEntry& b) {
            return a.lastOpened > b.lastOpened;
        });
}

void ProjectManagerWindow::SaveProjects() {
    // 只保存用户注册的项目；引擎安装目录本身不是项目。
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : m_projects) {
        nlohmann::json item;
        item["name"] = e.name;
        item["path"] = ProjectManager::GetInstance().ToProjectRelativePath(e.path);
        item["lastOpened"] = e.lastOpened;
        arr.push_back(item);
    }
    nlohmann::json j;
    j["projects"] = arr;

    std::string path = ProjectsFilePath();
    std::ofstream out(Utf8Path(path));
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
        if (CanonicalProjectKey(e.path) == CanonicalProjectKey(path)) {
            e.lastOpened = std::time(nullptr);
        }
    }
    SaveProjects();
    if (!MikanEngine_OpenProject(path.c_str())) {
        std::cerr << "[ProjectManagerWindow] Failed to open project: " << path << std::endl;
        m_visible = true;
        return;
    }
    m_visible = false;
    Editor::AssetsWindow::GetInstance().SetAssetsRootPath(ProjectManager::GetInstance().GetAssetsDir());
}

void ProjectManagerWindow::ImportProject() {
    const std::string manifestPath = OpenProjectManifestDialog();
    if (manifestPath.empty()) return;

    const std::filesystem::path projectPath =
        Utf8Path(manifestPath).parent_path();
    if (!DirectoryHasAssets(Utf8String(projectPath))) {
        SetNewError("选择的目录不是有效项目");
        return;
    }

    ProjectEntry entry;
    entry.path = Utf8String(projectPath);
    entry.name = Utf8String(projectPath.filename());
    try {
        std::ifstream in(Utf8Path(manifestPath));
        if (in.is_open()) {
            nlohmann::json j;
            in >> j;
            entry.name = j.value("name", entry.name);
        }
    } catch (const std::exception& ex) {
        SetNewError((std::string("项目清单解析失败: ") + ex.what()).c_str());
        return;
    }

    const std::string key = CanonicalProjectKey(entry.path);
    auto existing = std::find_if(m_projects.begin(), m_projects.end(),
        [&](const ProjectEntry& item) {
            return CanonicalProjectKey(item.path) == key;
        });
    if (existing == m_projects.end()) {
        m_projects.push_back(entry);
    } else {
        existing->name = entry.name;
    }
    SaveProjects();
    RefreshProjectList();
    OpenProject(entry.path);
}

// 右侧内容:项目列表(表格 + 打开/移除 + 新建)
void ProjectManagerWindow::RenderProjectListTab() {
    if (ImGui::BeginTable("ProjectTable", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("项目", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("路径", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableHeadersRow();

        std::string projectToRemove;
        for (const auto& e : m_projects) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", e.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", e.path.c_str());
            ImGui::TableSetColumnIndex(2);
            std::string openLabel = "打开##" + e.path;
            if (ImGui::Button(openLabel.c_str(), ImVec2(72, 0))) {
                OpenProject(e.path);
            }
            ImGui::SameLine();
            std::string delLabel = "移除##" + e.path;
            if (ImGui::Button(delLabel.c_str(), ImVec2(64, 0))) {
                projectToRemove = e.path;
            }
        }
        if (!projectToRemove.empty()) {
            const std::string removeKey = CanonicalProjectKey(projectToRemove);
            m_projects.erase(std::remove_if(m_projects.begin(), m_projects.end(),
                [&](const ProjectEntry& x) {
                    return CanonicalProjectKey(x.path) == removeKey;
                }), m_projects.end());
            SaveProjects();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("导入已有项目…", ImVec2(190, 0))) {
        ImportProject();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(选择 project.json)");

    ImGui::Spacing();

    // 新建项目
    if (!m_showNewDialog) {
        if (ImGui::Button("新建项目", ImVec2(144, 0))) {
            OpenNewDialog();
        }
    } else {
        ImGui::Text("新建项目");
        ImGui::InputText("项目名称", m_newName, ProjectManagerWindow::kNewNameSize);
        ImGui::InputText("项目路径", m_newPath, ProjectManagerWindow::kNewPathSize);
#ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::Button("浏览…", ImVec2(96, 0))) {
            const std::string selectedPath = OpenProjectParentDirectoryDialog();
            if (!selectedPath.empty()) {
                std::snprintf(m_newPath, sizeof(m_newPath), "%s", selectedPath.c_str());
                m_errorMsg[0] = '\0';
            }
        }
#endif
        ImGui::TextDisabled("创建位置：项目路径/项目名称");
        if (m_errorMsg[0]) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_errorMsg);
        }
        ImGui::Spacing();
        if (ImGui::Button("创建并打开", ImVec2(144, 0))) {
            std::string name = m_newName;
            std::string path = m_newPath;
            if (name.empty()) {
                SetNewError("项目名称不能为空");
            } else if (path.empty()) {
                SetNewError("项目路径不能为空");
            } else {
                std::string error;
                if (!ProjectManager::GetInstance().CreateProject(path, name, &error)) {
                    SetNewError(error.c_str());
                } else {
                    std::filesystem::path projectPath =
                        std::filesystem::absolute(
                            Utf8Path(path) /
                            Utf8Path(name));
                    ProjectEntry e;
                    e.name = name;
                    e.path = Utf8String(projectPath);
                    e.lastOpened = std::time(nullptr);
                    m_projects.push_back(e);
                    SaveProjects();
                    OpenProject(e.path);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(96, 0))) {
            CloseNewDialog();
        }
    }
}

void ProjectManagerWindow::Render() {
    if (!m_visible) return;

    // 项目管理器是启动页，优先保证远距离和高 DPI 显示器上的可读性。
    // 使用局部字体/间距缩放，不改变其他编辑器窗口的布局密度。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float baseFontSize = style.FontSizeBase > 0.0f ? style.FontSizeBase : 13.0f;
    ImGui::PushFont(ImGui::GetFont(), baseFontSize * kProjectManagerUiScale);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(7.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 20.0f);
    ImGui::Begin("项目管理器", nullptr, flags);

    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.9f, 1.0f), "MikanEngine - 项目管理器");
    ImGui::TextDisabled("选择要打开的项目,或新建一个项目");
    ImGui::Separator();
    ImGui::Spacing();

    // ===== 左右分栏:左侧菜单,右侧选中内容 =====
    const float leftW = 240.0f;

    ImGui::BeginChild("PM_Left", ImVec2(leftW, 0), true);
    ImGui::TextDisabled("菜单");
    ImGui::Separator();
    if (ImGui::Selectable("项目列表", m_selectedTab == 0)) m_selectedTab = 0;
    // 预留菜单项(模板 / 设置):未实现,置灰占位
    ImGui::BeginDisabled();
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
    ImGui::PopStyleVar(8);
    ImGui::PopFont();
}

} // namespace Editor
