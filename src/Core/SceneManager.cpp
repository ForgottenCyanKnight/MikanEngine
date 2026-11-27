// SceneManager.cpp - 运行时场景切换
#include "Core/Log.h"
#include "Core/LogStream.h"
#include "Core/SceneManager.h"
#include "Core/TilemapSystem.h"
#include "Core/ProjectManager.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/ScriptSystem.h"
#include "Game/GameManager.h"
#include "SceneSerializer.h"

SceneManager& SceneManager::GetInstance() {
    static SceneManager instance;
    return instance;
}

bool SceneManager::ChangeScene(const std::string& path) {
    // The serializer validates the replacement before clearing the live scene.
    const std::string full = ProjectManager::GetInstance().ResolveAssetPath(path);
    ECS::SceneSerializer loader;
    if (!loader.LoadScene(full)) {
        LOGSTREAM(Error) << "[SceneManager] load failed: " << full;
        return false;
    }

    // 加载瓦片地图(遍历场景中 TilemapComponent 实体)
    TilemapSystem::GetInstance().LoadAllFromScene();

    // 4. 按场景 "game" 键激活游戏模块(场景自带标记)
    auto sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
#if defined(__linux__) && !defined(__ANDROID__) && !defined(MIKAN_OPENHARMONY)
    // Linux builds the selected project's manifest module. Bind it again
    // after each scene change so it can resolve the new scene entities.
    if (sceneGame.empty()) {
        sceneGame = ProjectManager::GetInstance().GetManifest().game;
    }
#endif
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
    LOGSTREAM(Info) << "[SceneManager] scene changed -> " << path;
    return true;
}
