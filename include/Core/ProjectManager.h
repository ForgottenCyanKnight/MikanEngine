#pragma once
// ProjectManager.h - engine root + project root + asset path resolution (Core)
// Engine assets (shaders, fonts, default textures) always come from the engine
// install root; project assets (models, scenes, user textures) come from the
// project root. Supports:
//   --project <dir>   explicit project root (must contain assets/)
//   auto-detect       walk up from the exe dir to find the engine root;
//                     project root defaults to the engine root.
#include <string>
#include <vector>

// 项目清单（project.json，2026-08 项目化）：场景文件=项目工作目录配置，
// 资源区=项目目录本身（Unity 式）；assets[] 记录项目资源（新增资源时加入，构建/打包仅需必要文件）。
struct ProjectManifest {
    bool valid = false;
    std::string name;      // 项目名（项目列表显示）
    std::string scene;     // 场景文件，相对项目目录
    std::string game;      // 游戏插件名
    std::string resourceRoot; // 资源区（相对项目目录；默认 "."）
    std::vector<std::string> assets; // 资源清单（相对项目目录）
};

class ProjectManager {
public:
    static ProjectManager& GetInstance();

    // Parse argc/argv for --project; otherwise auto-detect from exe location.
    bool Initialize(int argc, char* argv[]);

    // 运行时切换项目根（项目管理器选择项目后调用）。
    // 项目化项目：目录含 project.json → 读清单，资源区=项目目录；
    // 旧式项目：目录含 assets/ → 资源区=assets/。
    bool SetProjectRoot(const std::string& dir);

    // Project asset path: <project>/<path>（项目化）或 <project>/assets/<path>（旧式）
    std::string ResolveAssetPath(const std::string& path) const;

    // Engine asset path: <engine>/assets/<path> (shaders, fonts, default textures)
    std::string GetEngineAssetPath(const std::string& path) const;

    bool IsExplicitProject() const { return m_explicitProject; }
    const std::string& GetProjectRoot() const { return m_projectRoot; }
    const std::string& GetEngineRoot() const { return m_engineRoot; }
    std::string GetAssetsDir() const { return m_assetsDir; }
    bool HasSceneConfig() const;
    std::string GetSceneConfigPath() const { return m_assetsDir + "sence.json"; }
    const ProjectManifest& GetManifest() const { return m_manifest; }
    bool HasManifest() const { return m_manifest.valid; }
    bool IsManifestProject() const { return m_manifest.valid; }

    // 项目资产白名单。项目化项目只允许清单 assets[] 中的文件/目录进入资产浏览器；
    // 旧式项目沿用 assets/ 目录作为完整资产边界。
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

    // 项目路径解析（2026-08，projects.json 相对路径支持）：
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
