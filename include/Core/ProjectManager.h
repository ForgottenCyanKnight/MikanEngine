#pragma once
// ProjectManager.h - engine root + selected project + asset path resolution.
// Engine assets (shaders, fonts, default textures) always come from the engine
// install root. Desktop project assets and gameplay code come from the selected
// project root; no project is selected implicitly.
// Supports:
//   --project <dir>   explicit project root (project.json)
//   auto-detect       walk up from the exe dir to find the engine root only.
#include <string>
#include <vector>

// 资源区=项目目录本身（Unity 式）；assets[] 记录项目资源（新增资源时加入，构建/打包仅需必要文件）。
struct ProjectManifest {
    bool valid = false;
    std::string name;      // 项目名（项目列表显示）
    std::string scene;     // 场景文件，相对项目目录
    std::string game;      // 游戏插件名
    std::string resourceRoot; // 资源区（相对项目目录；默认 "."）
    std::string codeRoot;     // 玩法源码根（相对项目目录；默认 "games"）
    std::vector<std::string> assets; // 资源清单（相对项目目录）
};

class ProjectManager {
public:
    static ProjectManager& GetInstance();

    // Parse argc/argv for --project; auto-detect only the engine install root.
    bool Initialize(int argc, char* argv[]);

    // 运行时切换项目根（项目管理器选择项目后调用）。
    // 项目目录必须含 project.json；资源区按清单中的 resourceRoot 解析。
    bool SetProjectRoot(const std::string& dir);

    // Project asset path: <project>/<path>（由 resourceRoot 决定）。
    // Desktop 未选择项目时，项目相对路径返回空字符串，禁止回退到引擎根/assets。
    std::string ResolveAssetPath(const std::string& path) const;

    // Engine asset path: <engine>/engine/<path> (shaders, fonts, default textures)
    std::string GetEngineAssetPath(const std::string& path) const;

    bool IsExplicitProject() const { return m_explicitProject; }
    bool HasActiveProject() const { return !m_projectRoot.empty(); }
    const std::string& GetProjectRoot() const { return m_projectRoot; }
    const std::string& GetEngineRoot() const { return m_engineRoot; }
    std::string GetAssetsDir() const { return m_assetsDir; }
    std::string GetCodeDir() const;
    const ProjectManifest& GetManifest() const { return m_manifest; }
    bool HasManifest() const { return m_manifest.valid; }
    bool IsManifestProject() const { return m_manifest.valid; }

    // 项目资产白名单：只允许清单 assets[] 中的文件/目录进入资产浏览器。
    bool IsProjectAsset(const std::string& path, bool directory) const;
    std::string GetProjectRelativePath(const std::string& path) const;
    bool RegisterProjectAsset(const std::string& path);
    bool UnregisterProjectAsset(const std::string& path);
    bool RenameProjectAsset(const std::string& oldPath, const std::string& newPath);
    bool SaveManifest();
    bool ReloadManifest();

    // 创建标准项目目录：project.json + scenes/main.json，返回创建出的项目目录是否成功。
    bool CreateProject(const std::string& parentDirectory,
                       const std::string& projectName,
                       std::string* errorMessage = nullptr);

    //   - 绝对路径 → 原样；相对路径 → 拼到引擎根（projects.json 与引擎根同目录）。
    //   发布目录移动后相对路径仍有效，绝对路径会失效。
    std::string ResolveProjectPath(const std::string& path) const;

    // 把引擎根内的绝对路径转成相对（存 projects.json 用）；根外路径保持绝对。
    std::string ToProjectRelativePath(const std::string& path) const;

private:
    ProjectManager() = default;
    std::string DetectEngineRoot() const;
    void LoadManifest(const std::string& manifestPath);
    bool m_explicitProject = false;
    std::string m_engineRoot;
    std::string m_projectRoot;
    std::string m_assetsDir;
    ProjectManifest m_manifest;
};
