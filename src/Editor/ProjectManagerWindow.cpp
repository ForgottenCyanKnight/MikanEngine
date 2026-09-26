// ProjectManagerWindow.cpp - 引擎项目管理器启动页
// 左右分栏:左侧项目导航,右侧项目列表内容。
// 打开项目调用 Game.dll 导出的 MikanEngine_OpenProject → 切换项目根 + 加载场景。
#include "Editor/ProjectManagerWindow.h"
#include "Core/Utf8Path.h"
#include "Editor/AssetsWindow.h"
#include "Editor/ControlPanelWindow.h"
#include "Editor/ToolbarWindow.h"
#include "PreviewGenerator.h"
#include "imgui/imgui.h"
#include "Core/I18n.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
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

struct ProjectTemplateInfo {
    const char* title;
    const char* description;
    const char* detail;
};

// 模板只描述用户可见的预设；实际内容由 CreateProjectFromTemplate 生成或复制。
constexpr ProjectTemplateInfo kProjectTemplates[] = {
    {"空白 3D 场景", "创建带主相机和方向光的干净 3D 项目。",
     "适合从零开始搭建 3D 场景、模型和玩法。"},
    {"空白 2D 场景", "创建 2D 相机和画布，不附带任何玩法对象。",
     "适合 UI、精灵、瓦片地图或自定义 2D 玩法。"},
    {"第三人称动画示例", "复制引擎自带的第三人称角色与动画示例。",
     "包含角色、动画、相机、物理和示例场景；创建后可直接编译 games。"},
    {"贪吃蛇 2D 模板", "创建一个可运行的 2D 贪吃蛇起始项目。",
     "包含场景化 UI、键盘方向控制、增长、食物和游戏结束逻辑。"}
};

constexpr int kProjectTemplateCount =
    static_cast<int>(sizeof(kProjectTemplates) / sizeof(kProjectTemplates[0]));

namespace fs = std::filesystem;

static std::string ComparableTemplatePath(const fs::path& path) {
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(path, ec);
    if (ec) normalized = path.lexically_normal();
    std::string value = GenericUtf8String(normalized);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (value.size() > 1 && (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }
    return value;
}

static bool IsTemplatePathInside(const fs::path& candidate, const fs::path& root) {
    const std::string candidateText = ComparableTemplatePath(candidate);
    const std::string rootText = ComparableTemplatePath(root);
    return !candidateText.empty() && !rootText.empty() &&
           (candidateText == rootText ||
            (candidateText.size() > rootText.size() &&
             candidateText.rfind(rootText + "/", 0) == 0));
}

static bool WriteUtf8File(const fs::path& path, const std::string& text,
                          std::string* errorMessage) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (errorMessage) *errorMessage = "无法创建模板目录: " + GenericUtf8String(path.parent_path());
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (errorMessage) *errorMessage = "无法写入模板文件: " + GenericUtf8String(path);
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out.good()) {
        if (errorMessage) *errorMessage = "模板文件写入失败: " + GenericUtf8String(path);
        return false;
    }
    return true;
}

static bool CopyTemplateFile(const fs::path& source, const fs::path& destination,
                             std::string* errorMessage) {
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec) {
        if (errorMessage) *errorMessage = "模板文件不存在: " + GenericUtf8String(source);
        return false;
    }
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        if (errorMessage) *errorMessage = "无法创建模板目录: " + GenericUtf8String(destination.parent_path());
        return false;
    }
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (errorMessage) {
            *errorMessage = "无法复制模板文件: " + GenericUtf8String(source) +
                            " -> " + GenericUtf8String(destination);
        }
        return false;
    }
    return true;
}

static bool CopyTemplateDirectory(const fs::path& source, const fs::path& destination,
                                  std::string* errorMessage) {
    std::error_code ec;
    if (!fs::is_directory(source, ec) || ec) {
        if (errorMessage) *errorMessage = "模板目录不存在: " + GenericUtf8String(source);
        return false;
    }
    fs::create_directories(destination, ec);
    if (ec) {
        if (errorMessage) *errorMessage = "无法创建模板项目目录: " + GenericUtf8String(destination);
        return false;
    }

    // 允许用户把新项目放在示例目录下，但不能让遍历器再次进入刚创建的
    // 目标目录，避免把目标复制到自身并不断扩张。
    const bool destinationInsideSource = IsTemplatePathInside(destination, source);

    fs::recursive_directory_iterator it(
        source, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::path current = it->path();
        const std::string fileName = GenericUtf8String(current.filename());
        if (destinationInsideSource && IsTemplatePathInside(current, destination)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            if (ec) break;
            it.increment(ec);
            continue;
        }
        if (it->is_directory(ec)) {
            if (fileName == "out" || fileName == "backup" || fileName == ".git") {
                it.disable_recursion_pending();
            } else {
                const fs::path relative = fs::relative(current, source, ec);
                if (ec) break;
                fs::create_directories(destination / relative, ec);
            }
        } else if (it->is_regular_file(ec)) {
            const fs::path relative = fs::relative(current, source, ec);
            if (ec) break;
            fs::create_directories((destination / relative).parent_path(), ec);
            if (!ec) {
                fs::copy_file(current, destination / relative,
                              fs::copy_options::overwrite_existing, ec);
            }
        }
        if (ec) break;
        it.increment(ec);
    }

    if (ec) {
        if (errorMessage) *errorMessage = "复制模板目录失败: " + GenericUtf8String(source);
        return false;
    }
    return true;
}

static nlohmann::json MakeTemplateEntity(std::uint32_t id, const char* name,
                                         const nlohmann::json& position) {
    nlohmann::json entity;
    entity["id"] = id;
    entity["name"] = {{"name", name}};
    entity["transform"] = {
        {"position", position},
        {"rotation", nlohmann::json::array({1.0f, 0.0f, 0.0f, 0.0f})},
        {"scale", nlohmann::json::array({1.0f, 1.0f, 1.0f})}
    };
    entity["hierarchy"] = {
        {"parent", 4294967295u},
        {"children", nlohmann::json::array()}
    };
    return entity;
}

static std::string BuildBlank3DScene() {
    nlohmann::json scene;
    scene["formatVersion"] = 1;
    scene["entities"] = nlohmann::json::array();

    // 主相机：位于 +Z 上方，带 ~11° 俯仰看向原点（atan2(1.1, 5.5)），
    // 四元数 [w,x,y,z] = 绕 X 轴 -11.31°，forward = rotation * (0,0,-1)。
    nlohmann::json camera = MakeTemplateEntity(
        0, "Main Camera", nlohmann::json::array({0.0f, 1.1f, 5.5f}));
    camera["transform"]["rotation"] = nlohmann::json::array(
        {0.9951f, -0.0986f, 0.0f, 0.0f});
    camera["camera"] = {
        {"fov", 60.0f},
        {"nearPlane", 0.1f},
        {"farPlane", 1000.0f},
        {"isMainCamera", true},
        {"isOrthographic", false},
        {"orthographicSize", 5.0f}
    };
    scene["entities"].push_back(camera);

    // 直射光：默认"上午"太阳高度（仰角 ~40°，而非恒等旋转的地平线日出），
    // 绕 X 轴 +40°，forward = rotation * (0,0,-1)。
    nlohmann::json light = MakeTemplateEntity(
        1, "Directional Light", nlohmann::json::array({0.0f, 6.0f, 4.0f}));
    light["transform"]["rotation"] = nlohmann::json::array(
        {0.9397f, 0.3420f, 0.0f, 0.0f});
    light["light"] = {
        {"type", 0},
        {"color", nlohmann::json::array({1.0f, 1.0f, 1.0f})},
        {"intensity", 1.0f},
        {"range", 10.0f},
        {"spotAngle", 45.0f},
        {"castShadow", true}
    };
    scene["entities"].push_back(light);

    // 天空盒：引擎默认 cubemap 资产（textureName = "skybox"）。
    nlohmann::json skybox = MakeTemplateEntity(
        2, "Skybox", nlohmann::json::array({0.0f, 0.0f, 0.0f}));
    skybox["skybox"] = {
        {"enabled", true},
        {"textureName", "skybox"},
        {"tint", nlohmann::json::array({1.0f, 1.0f, 1.0f})},
        {"intensity", 1.0f}
    };
    scene["entities"].push_back(skybox);
    return scene.dump(2) + "\n";
}

static std::string BuildBlank2DScene() {
    nlohmann::json scene;
    scene["formatVersion"] = 1;
    scene["entities"] = nlohmann::json::array();

    nlohmann::json camera = MakeTemplateEntity(
        0, "Camera2D", nlohmann::json::array({0.0f, 0.0f, 0.0f}));
    camera["camera2d"] = {
        {"enabled", true},
        {"center", nlohmann::json::array({0.0f, 0.0f})},
        {"zoom", 1.0f}
    };
    scene["entities"].push_back(camera);

    nlohmann::json canvas = MakeTemplateEntity(
        1, "Canvas", nlohmann::json::array({0.0f, 0.0f, 0.0f}));
    canvas["canvas2d"] = {
        {"width", 1920.0f},
        {"height", 1080.0f},
        {"stretchToViewport", true},
        {"layer", 0.0f}
    };
    scene["entities"].push_back(canvas);
    return scene.dump(2) + "\n";
}

static bool WriteProjectManifest(const fs::path& projectPath,
                                 const std::string& projectName,
                                 const std::string& scene,
                                 const std::string& game,
                                 const std::vector<std::string>& assets,
                                 std::string* errorMessage) {
    nlohmann::json manifest;
    manifest["formatVersion"] = 1;
    manifest["name"] = projectName;
    manifest["scene"] = scene;
    manifest["game"] = game;
    manifest["resourceRoot"] = ".";
    manifest["codeRoot"] = "games";
    manifest["editorPostProcessChain"] = "";
    manifest["assets"] = assets;
    return WriteUtf8File(projectPath / "project.json", manifest.dump(2) + "\n",
                         errorMessage);
}

static bool RenameCopiedProject(const fs::path& projectPath,
                                const std::string& projectName,
                                std::string* errorMessage) {
    const fs::path manifestPath = projectPath / "project.json";
    std::ifstream in(manifestPath);
    if (!in.is_open()) {
        if (errorMessage) *errorMessage = "模板缺少 project.json";
        return false;
    }
    try {
        nlohmann::json manifest;
        in >> manifest;
        manifest["name"] = projectName;
        return WriteUtf8File(manifestPath, manifest.dump(2) + "\n", errorMessage);
    } catch (const std::exception& ex) {
        if (errorMessage) *errorMessage = std::string("模板 project.json 解析失败: ") + ex.what();
        return false;
    }
}
}

// Game.dll 导出(由 Editor.dll 链接调用)
extern "C" __declspec(dllimport) bool MikanEngine_OpenProject(const char* dir);
extern "C" __declspec(dllimport) void MikanEngine_CloseProject();
extern "C" __declspec(dllimport) void MikanEngine_RequestViewportResolutionApply();

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
        LOGE("[ProjectManagerWindow] Failed to parse %s: %s", path.c_str(), ex.what());
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
        LOGI("[ProjectManagerWindow] Saved project list: %s", path.c_str());
    } else {
        LOGE("[ProjectManagerWindow] Failed to save project list: %s", path.c_str());
    }
}

void ProjectManagerWindow::RefreshProjectList() {
    LoadProjects();
}

void ProjectManagerWindow::OpenProjectManager() {
    m_controlPanelWasVisible = ControlPanelWindow::GetInstance().IsVisible();
    ControlPanelWindow::GetInstance().SetVisible(false);
    ToolbarWindow::GetInstance().SetGameRunning(false);
    ToolbarWindow::GetInstance().SetGamePaused(false);
    m_showNewDialog = false;
    m_pendingTemplate = -1;
    m_visible = true;

    // PreviewModelRenderer 使用材质描述符缓存的布局；项目切换会重建该布局，
    // 因此必须先释放编辑器预览管线，避免旧 pipeline 持有悬空 layout。
    PreviewGenerator::GetInstance().Cleanup();
    MikanEngine_CloseProject();
    AssetsWindow::GetInstance().SetAssetsRootPath({});
}

void ProjectManagerWindow::OpenProject(const std::string& path) {
    for (auto& e : m_projects) {
        if (CanonicalProjectKey(e.path) == CanonicalProjectKey(path)) {
            e.lastOpened = std::time(nullptr);
        }
    }
    SaveProjects();
    if (!MikanEngine_OpenProject(path.c_str())) {
        LOGE("[ProjectManagerWindow] Failed to open project: %s", path.c_str());
        m_visible = true;
        return;
    }
    m_visible = false;
    Editor::ControlPanelWindow::GetInstance().SetVisible(m_controlPanelWasVisible);
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

bool ProjectManagerWindow::CreateProjectFromTemplate(
    const std::string& parentDirectory, const std::string& projectName,
    int templateIndex, std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    if (templateIndex < 0) {
        return ProjectManager::GetInstance().CreateProject(
            parentDirectory, projectName, errorMessage);
    }
    if (templateIndex >= kProjectTemplateCount) {
        if (errorMessage) *errorMessage = "未知项目模板";
        return false;
    }

    const fs::path engineRoot = Utf8Path(ProjectManager::GetInstance().GetEngineRoot());
    const fs::path sourceRoot = templateIndex == 2
        ? (engineRoot / "projects" / "third-person-navigation")
        : (engineRoot / "projects" / "engine-samples");
    if ((templateIndex == 2 || templateIndex == 3) &&
        !fs::is_directory(sourceRoot)) {
        if (errorMessage) {
            *errorMessage = "引擎安装中缺少模板资源: " + GenericUtf8String(sourceRoot);
        }
        return false;
    }

    if (!ProjectManager::GetInstance().CreateProject(
            parentDirectory, projectName, errorMessage)) {
        return false;
    }

    std::error_code ec;
    const fs::path projectPath =
        fs::absolute(Utf8Path(parentDirectory) / Utf8Path(projectName), ec)
            .lexically_normal();
    if (ec || projectPath.empty()) {
        if (errorMessage) *errorMessage = "无法解析新项目路径";
        return false;
    }

    const auto failAndCleanup = [&](const std::string& message) {
        std::error_code cleanupError;
        fs::remove_all(projectPath, cleanupError);
        if (errorMessage) *errorMessage = message;
        return false;
    };

    std::string templateError;
    bool ok = false;
    switch (templateIndex) {
    case 0:
        ok = WriteUtf8File(projectPath / "scenes" / "main.json",
                           BuildBlank3DScene(), &templateError) &&
             WriteProjectManifest(projectPath, projectName, "scenes/main.json", "",
                                  {"scenes/main.json"}, &templateError);
        break;
    case 1:
        ok = WriteUtf8File(projectPath / "scenes" / "main.json",
                           BuildBlank2DScene(), &templateError) &&
             WriteProjectManifest(projectPath, projectName, "scenes/main.json", "",
                                  {"scenes/main.json"}, &templateError);
        break;
    case 2:
        ok = CopyTemplateDirectory(sourceRoot, projectPath, &templateError) &&
             RenameCopiedProject(projectPath, projectName, &templateError);
        break;
    case 3:
        ok = CopyTemplateFile(sourceRoot / "scenes" / "snake.json",
                              projectPath / "scenes" / "snake.json", &templateError) &&
             CopyTemplateFile(sourceRoot / "games" / "snake" / "SnakeGame.h",
                              projectPath / "games" / "snake" / "SnakeGame.h", &templateError) &&
             CopyTemplateFile(sourceRoot / "games" / "snake" / "SnakeGame.cpp",
                              projectPath / "games" / "snake" / "SnakeGame.cpp", &templateError) &&
             CopyTemplateFile(sourceRoot / "games" / "snake" / "GameSnakeExports.cpp",
                              projectPath / "games" / "snake" / "GameSnakeExports.cpp", &templateError) &&
             WriteProjectManifest(projectPath, projectName, "scenes/snake.json", "snake",
                                  {"scenes/snake.json"}, &templateError);
        break;
    default:
        break;
    }

    if (!ok) return failAndCleanup(templateError.empty() ? "创建模板项目失败" : templateError);
    return true;
}

// 右侧内容:固定操作栏 + 可滚动项目列表 + 新建项目表单
void ProjectManagerWindow::RenderProjectListTab() {
    // 操作栏位于列表之前，项目增多时不会被列表内容推到窗口底部。
    if (ImGui::Button(Tr("导入已有项目…"), ImVec2(190, 0))) {
        ImportProject();
    }
    ImGui::SameLine(0.0f, 10.0f);
    if (!m_showNewDialog) {
        if (ImGui::Button(Tr("新建项目"), ImVec2(144, 0))) {
            m_pendingTemplate = -1;
            OpenNewDialog();
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(Tr("新建项目"), ImVec2(144, 0));
        ImGui::EndDisabled();
    }

    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextDisabled(Tr("%zu 个项目"), m_projects.size());
    ImGui::Separator();

    // 新建表单也放在固定区，项目表滚动时不会带走操作控件。
    if (m_showNewDialog) {
        ImGui::Text(Tr("新建项目"));
        if (m_pendingTemplate >= 0 && m_pendingTemplate < kProjectTemplateCount) {
            ImGui::TextDisabled(Tr("模板：%s"), kProjectTemplates[m_pendingTemplate].title);
        }
        ImGui::InputText(Tr("项目名称"), m_newName, ProjectManagerWindow::kNewNameSize);
        ImGui::InputText(Tr("项目路径"), m_newPath, ProjectManagerWindow::kNewPathSize);
#ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::Button(Tr("浏览…"), ImVec2(96, 0))) {
            const std::string selectedPath = OpenProjectParentDirectoryDialog();
            if (!selectedPath.empty()) {
                std::snprintf(m_newPath, sizeof(m_newPath), "%s", selectedPath.c_str());
                m_errorMsg[0] = '\0';
            }
        }
#endif
        ImGui::TextDisabled(Tr("创建位置：项目路径/项目名称"));
        if (m_errorMsg[0]) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_errorMsg);
        }
        ImGui::Spacing();
        if (ImGui::Button(Tr("创建并打开"), ImVec2(144, 0))) {
            std::string name = m_newName;
            std::string path = m_newPath;
            if (name.empty()) {
                SetNewError("项目名称不能为空");
            } else if (path.empty()) {
                SetNewError("项目路径不能为空");
            } else {
                std::string error;
                if (!CreateProjectFromTemplate(path, name, m_pendingTemplate, &error)) {
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
                    m_pendingTemplate = -1;
                    OpenProject(e.path);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("取消"), ImVec2(96, 0))) {
            CloseNewDialog();
        }
        ImGui::Separator();
    }

    // 只有项目列表滚动；顶部导入/新建操作栏始终可见。
    ImGui::BeginChild("ProjectListScroll", ImVec2(0, 0), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (m_projects.empty()) {
        ImGui::TextDisabled(Tr("暂无项目，请从上方导入或新建项目。"));
    }

    if (ImGui::BeginTable("ProjectTable", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner |
            ImGuiTableFlags_SizingStretchProp)) {
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
    ImGui::EndChild();
}

void ProjectManagerWindow::RenderTemplatesTab() {
    ImGui::Text(Tr("项目模板"));
    ImGui::TextDisabled(Tr("选择一个预设，填写项目名称和位置后即可创建独立项目。"));
    ImGui::Separator();

    ImGui::BeginChild("TemplateListScroll", ImVec2(0, 0), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    // 说明区使用弹性列，操作区使用固定列，避免按钮随说明文字宽度变化而错位。
    if (ImGui::BeginTable("TemplateTable", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner |
            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(Tr("模板"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 180.0f);

        for (int i = 0; i < kProjectTemplateCount; ++i) {
            const ProjectTemplateInfo& info = kProjectTemplates[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(0.78f, 0.86f, 1.0f, 1.0f), "%s", info.title);
            ImGui::TextWrapped("%s", info.description);
            ImGui::TextDisabled("%s", info.detail);

            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button(Tr("使用此模板"), ImVec2(150, 0))) {
                m_pendingTemplate = i;
                m_selectedTab = 0;
                OpenNewDialog();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void ProjectManagerWindow::LoadSettingsDraft()
{
    m_settingsDraft = ProjectManager::GetInstance().GetEngineDisplaySettings();
    m_settingsLoaded = true;
    m_settingsStatusError = false;
    m_settingsStatus[0] = '\0';
}

void ProjectManagerWindow::SaveSettingsDraft()
{
    ProjectManager& projectManager = ProjectManager::GetInstance();
    const EngineDisplaySettings previous = projectManager.GetEngineDisplaySettings();
    std::string errorMessage;
    if (!projectManager.SetEngineDisplaySettings(m_settingsDraft, &errorMessage)) {
        m_settingsStatusError = true;
        snprintf(m_settingsStatus, sizeof(m_settingsStatus), "%s",
                 errorMessage.empty() ? "显示设置保存失败" : errorMessage.c_str());
        return;
    }

    m_settingsDraft = projectManager.GetEngineDisplaySettings();
    const bool viewportChanged =
        previous.viewportWidth != m_settingsDraft.viewportWidth ||
        previous.viewportHeight != m_settingsDraft.viewportHeight;
    if (viewportChanged) {
        MikanEngine_RequestViewportResolutionApply();
        snprintf(m_settingsStatus, sizeof(m_settingsStatus),
                 "设置已保存。引擎分辨率将在下次重启生效，视窗分辨率已请求立即应用。\n"
                 "输入值已限制在 640x360 至 7680x4320 范围内。");
    } else {
        snprintf(m_settingsStatus, sizeof(m_settingsStatus),
                 "设置已保存。引擎分辨率将在下次重启生效。\n"
                 "输入值已限制在 640x360 至 7680x4320 范围内。");
    }
    m_settingsStatusError = false;
}

void ProjectManagerWindow::RenderSettingsTab()
{
    if (!m_settingsLoaded) LoadSettingsDraft();

    ImGui::Text(Tr("设置"));
    ImGui::TextDisabled(Tr("配置会保存到引擎根目录的 engine_settings.json。"));
    ImGui::Separator();

    ImGui::BeginChild("SettingsScroll", ImVec2(0, 0), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::SeparatorText(Tr("引擎分辨率"));
    ImGui::TextWrapped(
        "启动窗口的客户区和 Vulkan 交换链尺寸。保存后不会立即调整当前窗口，"
        "下次启动引擎时生效。");
    ImGui::PushItemWidth(220.0f);
    ImGui::InputInt(Tr("宽度##EngineResolution"), &m_settingsDraft.engineWidth, 16, 160);
    ImGui::InputInt(Tr("高度##EngineResolution"), &m_settingsDraft.engineHeight, 16, 90);
    ImGui::PopItemWidth();
    ImGui::TextDisabled(Tr("范围：640x360 至 7680x4320；默认：1920x1040。"));

    ImGui::Spacing();
    ImGui::SeparatorText(Tr("视窗分辨率"));
    ImGui::TextWrapped(
        "SceneView 和 GameView 使用的内部渲染目标尺寸。默认是 1920x1080，"
        "保存后会在安全的帧边界请求资源重建并立即应用。");
    ImGui::PushItemWidth(220.0f);
    ImGui::InputInt(Tr("宽度##ViewportResolution"), &m_settingsDraft.viewportWidth, 16, 160);
    ImGui::InputInt(Tr("高度##ViewportResolution"), &m_settingsDraft.viewportHeight, 16, 90);
    ImGui::PopItemWidth();
    ImGui::TextDisabled(Tr("范围：640x360 至 7680x4320；默认：1920x1080。"));

    ImGui::Spacing();
    if (ImGui::Button(Tr("保存设置"), ImVec2(144, 0))) {
        SaveSettingsDraft();
    }
    ImGui::SameLine(0.0f, 10.0f);
    if (ImGui::Button(Tr("恢复默认"), ImVec2(144, 0))) {
        m_settingsDraft = EngineDisplaySettings{};
        m_settingsStatusError = false;
        snprintf(m_settingsStatus, sizeof(m_settingsStatus),
                 "已恢复默认值，点击“保存设置”后写入配置文件。");
    }
    if (m_settingsStatus[0] != '\0') {
        ImGui::Spacing();
        const ImVec4 color = m_settingsStatusError
            ? ImVec4(1.0f, 0.45f, 0.40f, 1.0f)
            : ImVec4(0.45f, 0.90f, 0.60f, 1.0f);
        ImGui::TextColored(color, "%s", m_settingsStatus);
    }
    ImGui::EndChild();
}

void ProjectManagerWindow::RenderAboutTab()
{
    ImGui::Text(Tr("关于 Mikan Engine"));
    ImGui::TextDisabled(Tr("个人开发者游戏引擎项目"));
    ImGui::Separator();

    ImGui::BeginChild("AboutScroll", ImVec2(0, 0), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextColored(ImVec4(0.78f, 0.86f, 1.0f, 1.0f), "Mikan Engine");
    ImGui::Text(Tr("面向个人开发的 C++ Vulkan 游戏引擎与编辑器。"));
    ImGui::TextWrapped(
        "项目目标是把场景编辑、资源管理、渲染、物理和玩法模块整合到一套"
        "轻量、可持续演进的开发工作流中。");

    ImGui::Spacing();
    ImGui::SeparatorText(Tr("开发者"));
    ImGui::Text(Tr("开发者：被遗忘的青色剑士"));
    ImGui::TextDisabled(Tr("负责引擎架构、渲染、编辑器和工具链的设计与实现。"));

    ImGui::Spacing();
    ImGui::SeparatorText(Tr("技术栈"));
    ImGui::TextWrapped("C++20 · Vulkan · SDL3 · Dear ImGui · ECS · Jolt Physics · Box2D");

    ImGui::Spacing();
    ImGui::SeparatorText(Tr("项目状态"));
    ImGui::Text(Tr("持续开发中"));
    ImGui::TextDisabled(Tr("感谢使用 Mikan Engine。"));
    ImGui::EndChild();
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
    ImGui::Begin(I18n::WindowTitle("项目管理器", "editor.project_manager").c_str(), nullptr, flags);

    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.9f, 1.0f), "MikanEngine - 项目管理器");
    ImGui::TextDisabled(Tr("选择要打开的项目,或新建一个项目"));
    ImGui::Separator();
    ImGui::Spacing();

    // ===== 左右分栏:左侧菜单,右侧选中内容 =====
    const float leftW = 240.0f;

    ImGui::BeginChild("PM_Left", ImVec2(leftW, 0), true);
    ImGui::TextDisabled(Tr("菜单"));
    ImGui::Separator();
    if (ImGui::Selectable(Tr("项目列表"), m_selectedTab == 0)) m_selectedTab = 0;
    if (ImGui::Selectable(Tr("模板"), m_selectedTab == 1)) m_selectedTab = 1;
    if (ImGui::Selectable(Tr("设置"), m_selectedTab == 2)) m_selectedTab = 2;
    if (ImGui::Selectable(Tr("关于"), m_selectedTab == 3)) m_selectedTab = 3;
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("PM_Right", ImVec2(0, 0), false);
    if (m_selectedTab == 0) {
        if (!m_loadedOnce) {
            LoadProjects();
            m_loadedOnce = true;
        }
        RenderProjectListTab();
    } else if (m_selectedTab == 1) {
        RenderTemplatesTab();
    } else if (m_selectedTab == 2) {
        RenderSettingsTab();
    } else if (m_selectedTab == 3) {
        RenderAboutTab();
    } else {
        ImGui::TextDisabled(Tr("该功能即将推出"));
    }
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleVar(8);
    ImGui::PopFont();
}

} // namespace Editor
