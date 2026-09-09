// ProjectManager.cpp - engine root + project root + asset path resolution
#include "Core/ProjectManager.h"
#include "Core/Utf8Path.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>
#include "json.hpp"

namespace {

namespace fs = std::filesystem;

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

fs::path AbsoluteNormalizedPath(const std::string& value, std::error_code& ec) {
    fs::path path = fs::absolute(Utf8Path(value), ec);
    if (ec) return {};
    return path.lexically_normal();
}

std::string ComparablePathText(const fs::path& path) {
    std::string result = LowerAscii(GenericUtf8String(path.lexically_normal()));
    // m_projectRoot/m_assetsDir are intentionally stored with a trailing '/'.
    // Remove it before adding the separator used by the descendant check.
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    return result;
}

bool IsSameOrDescendantPath(const fs::path& candidate, const fs::path& root) {
    const std::string candidateText = ComparablePathText(candidate);
    const std::string rootText = ComparablePathText(root);
    if (candidateText.empty() || rootText.empty()) return false;
    if (candidateText == rootText) return true;
    if (rootText == "/") return candidateText.front() == '/';
    return candidateText.size() > rootText.size() &&
           candidateText.rfind(rootText + "/", 0) == 0;
}

bool IsSameOrDescendantRelative(const std::string& candidate, const std::string& root) {
    const std::string candidateText = LowerAscii(candidate);
    const std::string rootText = LowerAscii(root);
    return candidateText == rootText ||
           (candidateText.size() > rootText.size() &&
            candidateText.rfind(rootText + "/", 0) == 0);
}

std::string NormalizeManifestPath(const std::string& value) {
    if (value.empty()) return {};
    fs::path path = Utf8Path(value).lexically_normal();
    if (path.is_absolute()) return {};
    std::string normalized = GenericUtf8String(path);
    while (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);
    if (normalized == "." || normalized.empty() ||
        normalized == ".." || normalized.rfind("../", 0) == 0) {
        return {};
    }
    return normalized;
}

std::string WithTrailingSlash(const fs::path& path) {
    std::string result = GenericUtf8String(path.lexically_normal());
    if (!result.empty() && result.back() != '/') result.push_back('/');
    return result;
}

fs::path ResolveResourceRoot(const std::string& projectRoot,
                             const std::string& resourceRoot) {
    std::error_code ec;
    const fs::path root = AbsoluteNormalizedPath(projectRoot, ec);
    if (ec || root.empty()) return {};

    const std::string normalizedResourceRoot =
        NormalizeManifestPath(resourceRoot.empty() ? "." : resourceRoot);
    const fs::path candidate = normalizedResourceRoot.empty()
        ? root
        : (root / Utf8Path(normalizedResourceRoot)).lexically_normal();
    return IsSameOrDescendantPath(candidate, root) ? candidate : root;
}

} // namespace

ProjectManager& ProjectManager::GetInstance() {
    static ProjectManager instance;
    return instance;
}

std::string ProjectManager::DetectEngineRoot() const {
    // Walk up from the exe dir, skipping build output dirs. The engine root is
    // the first directory whose engine/ contains compiled shaders (spv).
    // Build-output copies (x64-Release/engine) lack spv, so they are skipped.
    const char* base = SDL_GetBasePath();
    if (base) {
        // SDL returns UTF-8.  Constructing a Windows path from char* would
        // reinterpret Chinese characters through the active ANSI code page.
        std::filesystem::path p = Utf8Path(base);
        while (!p.empty()) {
            const std::string dir = Utf8String(p);
            const std::string lower = LowerAscii(GenericUtf8String(p));
            bool isBuildDir = lower.find("out/build") != std::string::npos ||
                              lower.find("x64-release") != std::string::npos ||
                              lower.find("x64-debug") != std::string::npos;
            if (!isBuildDir && std::filesystem::exists(
                    p / "engine" / "shaders" / "spv")) {
                return WithTrailingSlash(p);
            }
            const std::filesystem::path parent = p.parent_path();
            if (parent.empty() || parent == p) break;
            p = parent;
        }
    }
    return "";
}

bool ProjectManager::Initialize(int argc, char* argv[]) {
#ifdef __ANDROID__
    m_engineRoot = "";
    m_projectRoot = "";
    m_assetsDir = "";
    return true;
#else
    m_manifest = ProjectManifest{};
    m_explicitProject = false;
    m_projectRoot.clear();
    m_assetsDir.clear();

    // 1. Engine root: always auto-detected (contains compiled shaders)
    m_engineRoot = DetectEngineRoot();
    if (m_engineRoot.empty()) {
        std::cerr << "[ProjectManager] Could not locate engine root (engine/shaders/spv)." << std::endl;
        m_engineRoot = "";
    }

    // 2. Project root: only --project selects a desktop project. The engine
    // install root is never treated as a project or asset root implicitly.
    std::string projectArg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i] ? argv[i] : "";
        if (a == "--project" && i + 1 < argc) { projectArg = argv[i + 1] ? argv[i + 1] : ""; i++; }
        else if (a.rfind("--project=", 0) == 0) { projectArg = a.substr(10); }
    }
    if (!projectArg.empty()) {
        if (!SetProjectRoot(projectArg)) {
            std::cerr << "[ProjectManager] invalid --project directory: " << projectArg << std::endl;
            return false;
        }
        std::cout << "[ProjectManager] Project root (explicit): " << m_projectRoot << std::endl;
    }

    if (m_projectRoot.empty()) {
        std::cout << "[ProjectManager] No desktop project selected; waiting for project manager or --project." << std::endl;
    }
    return !m_engineRoot.empty();
#endif
}

bool ProjectManager::SetProjectRoot(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    const std::filesystem::path projectPath = AbsoluteNormalizedPath(dir, ec);
    if (ec || projectPath.empty()) return false;
    const std::string d = WithTrailingSlash(projectPath);

    const ProjectManifest previousManifest = m_manifest;
    const bool previousExplicitProject = m_explicitProject;
    const std::string previousProjectRoot = m_projectRoot;
    const std::string previousAssetsDir = m_assetsDir;
    const auto restorePreviousProject = [&]() {
        m_manifest = previousManifest;
        m_explicitProject = previousExplicitProject;
        m_projectRoot = previousProjectRoot;
        m_assetsDir = previousAssetsDir;
    };

    m_manifest = ProjectManifest{}; // 切换项目时重置清单

    const std::string manifestPath = d + "project.json";
    if (std::filesystem::exists(Utf8Path(manifestPath))) {
        LoadManifest(manifestPath);
        if (!m_manifest.valid) {
            restorePreviousProject();
            return false;
        }
        m_projectRoot = d;
        m_assetsDir = WithTrailingSlash(ResolveResourceRoot(d, m_manifest.resourceRoot));
        m_explicitProject = true;
        std::cout << "[ProjectManager] Project root (manifest): " << m_projectRoot
                  << " scene=" << m_manifest.scene << " game=" << m_manifest.game << std::endl;
        return true;
    }

    restorePreviousProject();
    std::cerr << "[ProjectManager] SetProjectRoot: project.json not found: " << d << std::endl;
    return false;
}

std::string ProjectManager::ResolveProjectPath(const std::string& path) const {
    if (path.empty()) return path;
    // Windows 盘符或 UNC/绝对：原样
    if (path.size() > 1 && path[1] == ':') return path;
    if (path[0] == '/' || path[0] == '\\') return path;
    // 相对 → 拼引擎根
    std::string root = m_engineRoot.empty() ? std::string() : m_engineRoot;
    return root + path;
}

std::string ProjectManager::ToProjectRelativePath(const std::string& path) const {
    if (path.empty() || m_engineRoot.empty()) return path;
    std::error_code ec;
    std::filesystem::path abs =
        std::filesystem::absolute(Utf8Path(path), ec);
    std::filesystem::path root =
        std::filesystem::absolute(Utf8Path(m_engineRoot), ec);
    if (ec) return path;
    std::string absStr = GenericUtf8String(abs.lexically_normal());
    std::string rootStr = GenericUtf8String(root.lexically_normal());
    // 大小写不敏感前缀匹配（Windows）
    std::string aLower = absStr, rLower = rootStr;
    aLower = LowerAscii(std::move(aLower));
    rLower = LowerAscii(std::move(rLower));
    if (aLower.rfind(rLower, 0) == 0 && aLower.size() > rLower.size()) {
        std::string rel = absStr.substr(rootStr.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        return rel; // 如 "projects/example"
    }
    return path; // 引擎根外：保持绝对
}

void ProjectManager::LoadManifest(const std::string& manifestPath) {
    std::ifstream in(Utf8Path(manifestPath));
    if (!in.is_open()) return;
    try {
        nlohmann::json j;
        in >> j;
        ProjectManifest loaded;
        loaded.valid = true;
        loaded.name = j.value("name", std::string());
        loaded.scene = NormalizeManifestPath(j.value("scene", std::string()));
        loaded.game = j.value("game", std::string());
        loaded.resourceRoot = NormalizeManifestPath(
            j.value("resourceRoot", std::string(".")));
        if (loaded.resourceRoot.empty()) loaded.resourceRoot = ".";
        loaded.codeRoot = NormalizeManifestPath(
            j.value("codeRoot", std::string("games")));
        if (loaded.codeRoot.empty()) loaded.codeRoot = "games";

        std::set<std::string> uniqueAssets;
        for (const auto& a : j.value("assets", nlohmann::json::array())) {
            if (!a.is_string()) continue;
            const std::string asset = NormalizeManifestPath(a.get<std::string>());
            if (!asset.empty() && uniqueAssets.insert(LowerAscii(asset)).second) {
                loaded.assets.push_back(asset);
            }
        }
        std::sort(loaded.assets.begin(), loaded.assets.end());
        m_manifest = std::move(loaded);
        std::cout << "[ProjectManager] Manifest loaded: " << manifestPath
                  << " (assets=" << m_manifest.assets.size() << ")" << std::endl;
    } catch (const std::exception& ex) {
        m_manifest = ProjectManifest{};
        std::cerr << "[ProjectManager] Failed to parse manifest " << manifestPath << ": " << ex.what() << std::endl;
    }
}

std::string ProjectManager::ResolveAssetPath(const std::string& path) const {
#ifdef __ANDROID__
    if (path.rfind("engine/", 0) == 0) return path.substr(7);
    return path;
#else
    if (path.empty()) return path;
    if (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'))
        return path;
    std::string p = path;
    while (p.rfind("../", 0) == 0) p = p.substr(3);
    if (p.rfind("engine/", 0) == 0)
        return m_engineRoot.empty() ? p : m_engineRoot + p;
    if (m_projectRoot.empty()) {
        // A desktop relative project path is meaningful only after a project
        // has been selected. Returning an empty path prevents accidental
        // reads from the process working directory or the engine checkout.
        return {};
    }
    if (p.rfind("assets/", 0) == 0)
        return m_projectRoot + p;
    return m_assetsDir.empty() ? std::string() : m_assetsDir + p;
#endif
}

std::string ProjectManager::GetEngineAssetPath(const std::string& path) const {
#ifdef __ANDROID__
    // Android：APK assets 根。sync_assets.ps1 已把 engine 资产拍平同步进 APK assets 根（无 engine/ 嵌套）
    return path;
#else
    if (path.empty()) return path;
    if (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'))
        return path;
    std::string p = path;
    while (p.rfind("../", 0) == 0) p = p.substr(3);
    if (p.rfind("engine/", 0) == 0)
        return m_engineRoot.empty() ? p : m_engineRoot + p;
    std::string engineAssets = m_engineRoot.empty() ? std::string("engine/") : (m_engineRoot + "engine/");
    return engineAssets + p;
#endif
}

std::string ProjectManager::GetCodeDir() const {
    if (m_projectRoot.empty()) return {};
    const std::string codeRoot =
        m_manifest.codeRoot.empty() ? std::string("games") : m_manifest.codeRoot;
    return m_projectRoot + codeRoot + "/";
}

std::string ProjectManager::GetProjectRelativePath(const std::string& path) const {
    if (path.empty() || m_projectRoot.empty()) return {};

    std::error_code ec;
    const fs::path candidate = AbsoluteNormalizedPath(path, ec);
    if (ec || candidate.empty()) return {};
    const fs::path root = AbsoluteNormalizedPath(m_projectRoot, ec);
    if (ec || root.empty() || !IsSameOrDescendantPath(candidate, root)) return {};

    const fs::path relative = candidate.lexically_relative(root);
    std::string result = GenericUtf8String(relative);
    if (result.empty() || result == ".") return {};
    if (result == ".." || result.rfind("../", 0) == 0) return {};
    return result;
}

bool ProjectManager::IsProjectAsset(const std::string& path, bool directory) const {
    if (path.empty() || m_assetsDir.empty()) return false;

    std::error_code ec;
    const fs::path candidate = AbsoluteNormalizedPath(path, ec);
    if (ec || candidate.empty()) return false;
    const fs::path assetRoot = AbsoluteNormalizedPath(m_assetsDir, ec);
    if (ec || assetRoot.empty() || !IsSameOrDescendantPath(candidate, assetRoot)) {
        return false;
    }

    const std::string relative = GetProjectRelativePath(path);
    if (relative.empty()) return directory;

    for (const auto& asset : m_manifest.assets) {
        // 清单中的目录覆盖其所有后代文件；清单中的文件只匹配自身。
        if (IsSameOrDescendantRelative(relative, asset)) return true;
        // 目录只要是某个已登记文件/目录的祖先，就应显示在目录树中。
        if (directory && IsSameOrDescendantRelative(asset, relative)) return true;
    }
    return false;
}

bool ProjectManager::RegisterProjectAsset(const std::string& path) {
    if (path.empty() || m_assetsDir.empty()) return false;

    std::error_code ec;
    const fs::path candidate = AbsoluteNormalizedPath(path, ec);
    const fs::path assetRoot = AbsoluteNormalizedPath(m_assetsDir, ec);
    if (ec || candidate.empty() || assetRoot.empty() ||
        !IsSameOrDescendantPath(candidate, assetRoot) ||
        !fs::exists(candidate, ec)) {
        return false;
    }

    const std::string relative = GetProjectRelativePath(path);
    if (relative.empty()) return false;
    for (const auto& asset : m_manifest.assets) {
        if (LowerAscii(asset) == LowerAscii(relative) ||
            IsSameOrDescendantRelative(relative, asset)) {
            return true;
        }
    }

    const std::vector<std::string> previous = m_manifest.assets;
    m_manifest.assets.push_back(relative);
    std::sort(m_manifest.assets.begin(), m_manifest.assets.end());
    if (!SaveManifest()) {
        m_manifest.assets = previous;
        return false;
    }
    return true;
}

bool ProjectManager::UnregisterProjectAsset(const std::string& path) {
    if (!m_manifest.valid || m_projectRoot.empty()) return false;
    const std::string relative = GetProjectRelativePath(path);
    if (relative.empty()) return false;

    const std::vector<std::string> previous = m_manifest.assets;
    m_manifest.assets.erase(
        std::remove_if(m_manifest.assets.begin(), m_manifest.assets.end(),
            [&](const std::string& asset) {
                return LowerAscii(asset) == LowerAscii(relative) ||
                       IsSameOrDescendantRelative(asset, relative);
            }),
        m_manifest.assets.end());
    if (m_manifest.assets == previous) return true;
    if (!SaveManifest()) {
        m_manifest.assets = previous;
        return false;
    }
    return true;
}

bool ProjectManager::RenameProjectAsset(const std::string& oldPath,
                                        const std::string& newPath) {
    if (!m_manifest.valid || m_projectRoot.empty()) return false;

    const std::string oldRelative = GetProjectRelativePath(oldPath);
    const std::string newRelative = GetProjectRelativePath(newPath);
    if (oldRelative.empty() || newRelative.empty()) return false;

    const std::vector<std::string> previous = m_manifest.assets;
    bool changed = false;
    for (auto& asset : m_manifest.assets) {
        const std::string lowerAsset = LowerAscii(asset);
        const std::string lowerOld = LowerAscii(oldRelative);
        if (lowerAsset == lowerOld) {
            asset = newRelative;
            changed = true;
        } else if (IsSameOrDescendantRelative(asset, oldRelative)) {
            asset = newRelative + asset.substr(oldRelative.size());
            changed = true;
        }
    }

    if (!changed) return true;
    std::sort(m_manifest.assets.begin(), m_manifest.assets.end());
    if (!SaveManifest()) {
        m_manifest.assets = previous;
        return false;
    }
    return true;
}

bool ProjectManager::SaveManifest() {
    if (!m_manifest.valid || m_projectRoot.empty()) return false;

    try {
        std::sort(m_manifest.assets.begin(), m_manifest.assets.end());
        m_manifest.assets.erase(
            std::unique(m_manifest.assets.begin(), m_manifest.assets.end(),
                [](const std::string& a, const std::string& b) {
                    return LowerAscii(a) == LowerAscii(b);
                }),
            m_manifest.assets.end());

        nlohmann::json j;
        j["name"] = m_manifest.name;
        j["scene"] = m_manifest.scene;
        j["game"] = m_manifest.game;
        j["resourceRoot"] = m_manifest.resourceRoot.empty() ? "." : m_manifest.resourceRoot;
        j["codeRoot"] = m_manifest.codeRoot.empty() ? "games" : m_manifest.codeRoot;
        j["assets"] = m_manifest.assets;

        const fs::path manifestPath = Utf8Path(m_projectRoot) / "project.json";
        std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[ProjectManager] Failed to save manifest: "
                      << Utf8String(manifestPath) << std::endl;
            return false;
        }
        out << j.dump(2) << "\n";
        const bool ok = out.good();
        out.close();
        if (ok) {
            std::cout << "[ProjectManager] Manifest saved: "
                      << Utf8String(manifestPath)
                      << " (assets=" << m_manifest.assets.size() << ")" << std::endl;
        }
        return ok;
    } catch (const std::exception& ex) {
        // Keep editor actions recoverable: an invalid path/JSON encoding must
        // reject the manifest update instead of escaping into the main loop.
        std::cerr << "[ProjectManager] Failed to save manifest: "
                  << ex.what() << std::endl;
        return false;
    }
}

bool ProjectManager::ReloadManifest() {
    if (!m_manifest.valid || m_projectRoot.empty()) return false;
    const ProjectManifest previous = m_manifest;
    m_manifest = ProjectManifest{};
    LoadManifest(m_projectRoot + "project.json");
    if (!m_manifest.valid) {
        m_manifest = previous;
        return false;
    }
    m_assetsDir = WithTrailingSlash(
        ResolveResourceRoot(m_projectRoot, m_manifest.resourceRoot));
    return true;
}

bool ProjectManager::CreateProject(const std::string& parentDirectory,
                                   const std::string& projectName,
                                   std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    const auto fail = [&](const std::string& message) {
        if (errorMessage) *errorMessage = message;
        return false;
    };

    if (parentDirectory.empty()) return fail("项目父目录不能为空");
    if (projectName.empty() || projectName == "." || projectName == ".." ||
        projectName.find('/') != std::string::npos ||
        projectName.find('\\') != std::string::npos) {
        return fail("项目名称包含非法路径字符");
    }

    std::error_code ec;
    const fs::path parent = AbsoluteNormalizedPath(parentDirectory, ec);
    if (ec || parent.empty()) return fail("项目父目录无效");
    const fs::path projectPath =
        (parent / Utf8Path(projectName)).lexically_normal();
    if (fs::exists(projectPath, ec)) return fail("项目目录已存在");

    try {
        fs::create_directories(projectPath / "scenes");
        fs::create_directories(projectPath / "games");

        std::ofstream scene(projectPath / "scenes" / "main.json",
                            std::ios::binary | std::ios::trunc);
        if (!scene.is_open()) throw std::runtime_error("无法创建默认场景");
        scene << "{\n  \"entities\": []\n}\n";
        scene.close();
        if (!scene.good()) throw std::runtime_error("默认场景写入失败");

        nlohmann::json manifest;
        manifest["name"] = projectName;
        manifest["scene"] = "scenes/main.json";
        manifest["game"] = "";
        manifest["resourceRoot"] = ".";
        manifest["codeRoot"] = "games";
        manifest["assets"] = nlohmann::json::array({"scenes/main.json"});

        std::ofstream project(projectPath / "project.json",
                              std::ios::binary | std::ios::trunc);
        if (!project.is_open()) throw std::runtime_error("无法创建 project.json");
        project << manifest.dump(2) << "\n";
        project.close();
        if (!project.good()) throw std::runtime_error("project.json 写入失败");
    } catch (const std::exception& ex) {
        std::error_code cleanupError;
        fs::remove_all(projectPath, cleanupError);
        return fail(std::string("创建项目失败: ") + ex.what());
    }

    return true;
}
