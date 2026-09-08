#pragma once
// SceneManager.h - 运行时场景切换(游戏事件/编辑器/启动共用入口)
// 统一流程: 清理旧场景运行时状态(2D 物理/瓦片碰撞/传感器补偿) → 加载新场景 →
// 加载瓦片地图 → 按场景 "game" 键激活游戏模块并 OnSceneLoaded → 通知回调。
// 游戏代码可随时调用: SceneManager::GetInstance().ChangeScene("scenes/level2.json");
#include "Platform/Export.h"
#include <functional>
#include <string>

class MIKAN_API SceneManager {
public:
    static SceneManager& GetInstance();

    // 切换场景(相对当前项目 resourceRoot 的路径, 如 "scenes/level2.json"); 成功返回 true
    bool ChangeScene(const std::string& path);

    // 当前场景路径(空 = 无)
    const std::string& GetCurrentScene() const { return m_currentScene; }

    // 场景切换完成回调(游戏可注册: 如保留跨场景全局状态/统计)
    void SetOnSceneChanged(std::function<void(const std::string&)> cb) { m_onSceneChanged = std::move(cb); }

private:
    SceneManager() = default;
    ~SceneManager() = default;
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    std::string m_currentScene;
    std::function<void(const std::string&)> m_onSceneChanged;
};
