// ProjectManager.cpp - engine root + project root + asset path resolution
#include "Core/ProjectManager.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include "json.hpp"

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
        std::filesystem::path p(base);
        while (p.has_parent_path()) {
            std::string dir = p.string();
            std::string lower = dir;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            bool isBuildDir = lower.find("out/build") != std::string::npos ||
                              lower.find("out\\build") != std::string::npos ||
                              lower.find("x64-release") != std::string::npos ||
                              lower.find("x64-debug") != std::string::npos;
            if (!isBuildDir && std::filesystem::exists(dir + "/engine/shaders/spv")) {
                if (dir.back() != '/' && dir.back() != '\\') dir += "/";
                return dir;
            }
            p = p.parent_path();
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
    // 1. Engine root: always auto-detected (contains compiled shaders)
    m_engineRoot = DetectEngineRoot();
    if (m_engineRoot.empty()) {
        std::cerr << "[ProjectManager] Could not locate engine root (engine/shaders/spv)." << std::endl;
        m_engineRoot = "";
    }

    // 2. Project root: --project <dir>, otherwise default to the engine root
    std::string projectArg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i] ? argv[i] : "";
        if (a == "--project" && i + 1 < argc) { projectArg = argv[i + 1] ? argv[i + 1] : ""; i++; }
        else if (a.rfind("--project=", 0) == 0) { projectArg = a.substr(10); }
    }
    if (!projectArg.empty()) {
        std::string pa = projectArg;
        if (pa.back() != '/' && pa.back() != '\\') pa += "/";
        const std::string manifestPath = pa + "project.json";
        if (std::filesystem::exists(manifestPath) || std::filesystem::exists(pa + "assets")) {
            if (std::filesystem::exists(manifestPath)) {
                LoadManifest(manifestPath);
                m_assetsDir = pa; // 项目化：资源区 = 项目根
            } else {
                m_assetsDir = pa + "assets/";
            }
            m_projectRoot = pa;
            m_explicitProject = true;
            std::cout << "[ProjectManager] Project root (explicit): " << m_projectRoot << std::endl;
            return true;
        }
        std::cerr << "[ProjectManager] --project dir has no assets/ or project.json: " << projectArg << std::endl;
    }

    // Default: project root = engine root (single-project layout)
    m_projectRoot = m_engineRoot;
    m_assetsDir = m_engineRoot.empty() ? std::string("assets/") : (m_engineRoot + "assets/");
    if (!m_engineRoot.empty())
        std::cout << "[ProjectManager] Project root (auto): " << m_projectRoot << std::endl;
    else
        std::cerr << "[ProjectManager] Using relative assets/ paths." << std::endl;
    return !m_engineRoot.empty();
#endif
}

bool ProjectManager::SetProjectRoot(const std::string& dir) {
    if (dir.empty()) return false;
    std::string d = dir;
    if (d.back() != '/' && d.back() != '\\') d += "/";

    m_manifest = ProjectManifest{}; // 切换项目时重置清单

    // 项目化项目（2026-08）：目录含 project.json → 场景=项目工作目录配置，资源区=项目目录本身
    const std::string manifestPath = d + "project.json";
    if (std::filesystem::exists(manifestPath)) {
        LoadManifest(manifestPath);
        m_projectRoot = d;
        m_assetsDir = d; // 资源区 = 项目根（Unity 式：项目目录即资源根）
        m_explicitProject = true;
        std::cout << "[ProjectManager] Project root (manifest): " << m_projectRoot
                  << " scene=" << m_manifest.scene << " game=" << m_manifest.game << std::endl;
        return true;
    }

    // 旧式项目：目录含 assets/（资源区 = assets/）
    if (std::filesystem::exists(d + "assets")) {
        m_projectRoot = d;
        m_assetsDir = d + "assets/";
        m_explicitProject = true;
        std::cout << "[ProjectManager] Project root (switched): " << m_projectRoot << std::endl;
        return true;
    }
    std::cerr << "[ProjectManager] SetProjectRoot: dir has neither project.json nor assets/: " << d << std::endl;
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
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    std::filesystem::path root = std::filesystem::absolute(m_engineRoot, ec);
    if (ec) return path;
    std::string absStr = abs.lexically_normal().generic_string();
    std::string rootStr = root.lexically_normal().generic_string();
    // 大小写不敏感前缀匹配（Windows）
    std::string aLower = absStr, rLower = rootStr;
    std::transform(aLower.begin(), aLower.end(), aLower.begin(), ::tolower);
    std::transform(rLower.begin(), rLower.end(), rLower.begin(), ::tolower);
    if (aLower.rfind(rLower, 0) == 0 && aLower.size() > rLower.size()) {
        std::string rel = absStr.substr(rootStr.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        return rel; // 如 "projects/baka3d"
    }
    return path; // 引擎根外：保持绝对
}

void ProjectManager::LoadManifest(const std::string& manifestPath) {
    std::ifstream in(manifestPath);
    if (!in.is_open()) return;
    try {
        nlohmann::json j;
        in >> j;
        m_manifest.valid = true;
        m_manifest.name = j.value("name", std::string());
        m_manifest.scene = j.value("scene", std::string());
        m_manifest.game = j.value("game", std::string());
        m_manifest.resourceRoot = j.value("resourceRoot", std::string("."));
        for (const auto& a : j.value("assets", nlohmann::json::array()))
            m_manifest.assets.push_back(a.get<std::string>());
        std::cout << "[ProjectManager] Manifest loaded: " << manifestPath
                  << " (assets=" << m_manifest.assets.size() << ")" << std::endl;
    } catch (const std::exception& ex) {
        m_manifest = ProjectManifest{};
        std::cerr << "[ProjectManager] Failed to parse manifest " << manifestPath << ": " << ex.what() << std::endl;
    }
}

std::string ProjectManager::ResolveAssetPath(const std::string& path) const {
#ifdef __ANDROID__
    return path;
#else
    if (path.empty()) return path;
    if (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'))
        return path;
    std::string p = path;
    while (p.rfind("../", 0) == 0) p = p.substr(3);
    if (p.rfind("assets/", 0) == 0)
        return m_projectRoot.empty() ? p : m_projectRoot + p;
    return m_assetsDir.empty() ? p : m_assetsDir + p;
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

bool ProjectManager::HasSceneConfig() const {
    return std::filesystem::exists(GetSceneConfigPath());
}