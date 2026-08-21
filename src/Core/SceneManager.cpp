// SceneManager.cpp - 运行时场景切换
#include "Core/SceneManager.h"
#include "Core/Physics2DSystem.h"
#include "Core/TilemapSystem.h"
#include "Core/ProjectManager.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/ScriptSystem.h"
#include "Game/GameManager.h"
#include "SceneSerializer.h"
#include <iostream>

SceneManager& SceneManager::GetInstance() {
    static SceneManager instance;
    return instance;
}

bool SceneManager::ChangeScene(const std::string& path) {
    // 1. 清理旧场景运行时状态(物理刚体 + 瓦片碰撞体 + 传感器补偿集合)
    Physics2DSystem::GetInstance().ClearBodies();
    TilemapSystem::GetInstance().ClearAll();

    // 2. 加载新场景(内部 ClearScene → 重建实体/组件)
    const std::string full = ProjectManager::GetInstance().ResolveAssetPath(path);
    ECS::SceneSerializer loader;
    if (!loader.LoadScene(full)) {
        std::cerr << "[SceneManager] load failed: " << full << std::endl;
        return false;
    }

    // 3. 加载瓦片地图(遍历场景中 TilemapComponent 实体)
    TilemapSystem::GetInstance().LoadAllFromScene();

    // 4. 按场景 "game" 键激活游戏模块(场景自带标记)
    const std::string& sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
    if (!sceneGame.empty()) {
        if (auto* gm = Game::GameManager::GetInstance().Activate(sceneGame)) {
            gm->OnSceneLoaded();
        }
    }

    // 4.5 幂等补齐脚本实例(游戏插件已激活并注册脚本工厂;编辑器/命令行任何加载路径都挂上场景脚本)
    ECS::ScriptSystem::GetInstance().InstantiateAll(false);

    // 5. 记录 + 通知回调
    m_currentScene = path;
    if (m_onSceneChanged) {
        m_onSceneChanged(path);
    }
    std::cout << "[SceneManager] scene changed -> " << path << std::endl;
    return true;
}
