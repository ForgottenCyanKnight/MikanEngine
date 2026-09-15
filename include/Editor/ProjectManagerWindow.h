#pragma once
// ProjectManagerWindow.h - 引擎项目管理器启动页
// 左右分栏:左侧项目导航,右侧项目列表内容。
// 项目注册表: <engineRoot>/projects.json
#include "Core/ProjectManager.h"
#include <string>
#include <vector>
#include <cstdio>

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
    void OpenProjectManager();         // 关闭当前项目并进入项目管理器
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
    void CloseNewDialog() {
        m_showNewDialog = false;
        m_pendingTemplate = -1;
    }
    char* NewNameBuffer() { return m_newName; }
    char* NewPathBuffer() { return m_newPath; }
    const char* NewErrorMsg() const { return m_errorMsg; }
    void SetNewError(const char* msg) {
        snprintf(m_errorMsg, sizeof(m_errorMsg), "%s", msg ? msg : "");
    }

private:
    ProjectManagerWindow() = default;
    void LoadProjects();   // 读 projects.json（不再注入引擎根 Default Project）
    void SaveProjects();   // 写回 projects.json
    void OpenProject(const std::string& path);
    void ImportProject();
    void RefreshProjectList();
    void RenderProjectListTab(); // 右侧"项目列表"内容(成员,可访问私有状态)
    void RenderTemplatesTab();    // 右侧"模板"内容
    void RenderSettingsTab();     // 右侧"设置"内容
    void RenderAboutTab();        // 右侧"关于"内容
    void LoadSettingsDraft();
    void SaveSettingsDraft();
    bool CreateProjectFromTemplate(const std::string& parentDirectory,
                                   const std::string& projectName,
                                   int templateIndex,
                                   std::string* errorMessage);

    std::vector<ProjectEntry> m_projects;
    // 启动时由显式项目参数或项目管理器选择打开；编辑器内可从“项目”菜单再次打开。
    bool m_visible = false;
    bool m_loadedOnce = false;
    bool m_controlPanelWasVisible = true;
    int m_selectedTab = 0; // 左侧菜单选中项: 0=项目列表

    // 新建项目弹窗状态
    bool m_showNewDialog = false;
    int m_pendingTemplate = -1; // -1=标准空项目, 0..N=模板页选中的预设
    char m_newName[kNewNameSize] = "";
    char m_newPath[kNewPathSize] = "";
    char m_errorMsg[256] = "";

    bool m_settingsLoaded = false;
    EngineDisplaySettings m_settingsDraft{};
    bool m_settingsStatusError = false;
    char m_settingsStatus[256] = "";
};

} // namespace Editor
