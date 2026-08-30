#pragma once
// ProjectManagerWindow.h - 引擎项目管理器启动页
// 左右分栏:左侧项目导航,右侧项目列表内容。
// 项目注册表: <engineRoot>/projects.json
#include <string>
#include <vector>

namespace Editor {

struct ProjectEntry {
    std::string name;
    std::string path;
    long long lastOpened = 0; // Unix 时间戳,0=未知
};

class ProjectManagerWindow {
public:
    static ProjectManagerWindow& GetInstance();

    void Render();                     // 全屏启动页或编辑器内项目管理器
    void SetVisible(bool v) { m_visible = v; }
    bool IsVisible() const { return m_visible; }

    // 新建项目弹窗访问(供右侧内容渲染使用)
    static constexpr int kNewNameSize = 128;
    static constexpr int kNewPathSize = 512;
    bool IsNewDialogOpen() const { return m_showNewDialog; }
    void OpenNewDialog() {
        m_showNewDialog = true;
        m_newName[0] = '\0';
        m_newPath[0] = '\0';
        m_errorMsg[0] = '\0';
    }
    void CloseNewDialog() { m_showNewDialog = false; }
    char* NewNameBuffer() { return m_newName; }
    char* NewPathBuffer() { return m_newPath; }
    const char* NewErrorMsg() const { return m_errorMsg; }
    void SetNewError(const char* msg) {
        snprintf(m_errorMsg, sizeof(m_errorMsg), "%s", msg ? msg : "");
    }

private:
    ProjectManagerWindow() = default;
    void LoadProjects();   // 读 projects.json(不存在时注册引擎根为默认项目)
    void SaveProjects();   // 写回 projects.json
    void OpenProject(const std::string& path);
    void ImportProject();
    void RefreshProjectList();
    void RenderProjectListTab(); // 右侧"项目列表"内容(成员,可访问私有状态)

    std::vector<ProjectEntry> m_projects;
    // 启动时只有没有显式项目才由 EditorDllApi 打开；编辑器内可从“项目”菜单再次打开。
    bool m_visible = false;
    bool m_loadedOnce = false;
    int m_selectedTab = 0; // 左侧菜单选中项: 0=项目列表

    // 新建项目弹窗状态
    bool m_showNewDialog = false;
    char m_newName[kNewNameSize] = "";
    char m_newPath[kNewPathSize] = "";
    char m_errorMsg[256] = "";
};

} // namespace Editor
