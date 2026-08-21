// GameManager.cpp - 游戏模块管理器
#include "Game/GameManager.h"

#include <iostream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Game {

GameManager& GameManager::GetInstance() {
    static GameManager instance;
    return instance;
}

void GameManager::Register(const std::string& name, GameFactory factory) {
    if (name.empty() || !factory) return;
    m_factories[name] = std::move(factory);
    std::cout << "[GameManager] Registered game module: " << name << std::endl;
}

bool GameManager::LoadPlugin(const std::string& name) {
    if (name.empty() || m_factories.count(name)) return true;
#ifdef _WIN32
    const std::string candidates[] = { name + ".dll", "Game" + name + ".dll" };
    HMODULE h = nullptr;
    std::string loadedPath;
    for (const auto& cand : candidates) {
        h = LoadLibraryA(cand.c_str());
        if (h) { loadedPath = cand; break; }
    }
    if (!h) {
        std::cerr << "[GameManager] LoadPlugin: no DLL for '" << name
                  << "' (tried " << candidates[0] << ", " << candidates[1] << ")" << std::endl;
        return false;
    }
    using GetNameFn = const char* (*)();
    using CreateFn  = IGameModule* (*)();
    auto getName = reinterpret_cast<GetNameFn>(GetProcAddress(h, "GetGameModuleName"));
    auto create  = reinterpret_cast<CreateFn>(GetProcAddress(h, "CreateGameModule"));
    if (!getName || !create) {
        std::cerr << "[GameManager] LoadPlugin: " << loadedPath
                  << " missing GetGameModuleName/CreateGameModule exports" << std::endl;
        FreeLibrary(h);
        return false;
    }
    std::string pluginName = getName() ? getName() : name;
    Register(pluginName, [create]() -> IGameModule* { return create(); });
    m_pluginHandles[pluginName] = h; // 记录句柄(供 ReloadPlugin 卸载)
    std::cout << "[GameManager] Loaded game plugin: " << loadedPath << " -> '" << pluginName << "'" << std::endl;
    return true;
#else
    (void)name;
    std::cerr << "[GameManager] LoadPlugin not supported on this platform" << std::endl;
    return false;
#endif
}

bool GameManager::ReloadPlugin(const std::string& name) {
#ifdef _WIN32
    auto fit = m_factories.find(name);
    if (fit == m_factories.end()) return false;          // 未加载过,无从重载
    auto hit = m_pluginHandles.find(name);
    if (hit == m_pluginHandles.end()) return false;      // 内置游戏(非插件)不支持热重载

    // 当前激活的是该插件 → 先停用(避免旧 DLL 对象继续被引用)
    if (m_current && m_current->GetName() == name) {
        Deactivate();
    }

    // 卸载旧 DLL 与旧工厂
    HMODULE oldHandle = static_cast<HMODULE>(hit->second);
    m_pluginHandles.erase(hit);
    m_factories.erase(fit);
    FreeLibrary(oldHandle); // 解锁文件(运行中的 DLL 被锁定无法覆盖)

    // 将编译产物 .tmp 移动为正式名(旧 DLL 已卸载,文件可替换;无 tmp 产物则忽略)
    {
        const std::string tmpPath   = "Game" + name + ".dll.tmp";
        const std::string finalPath = "Game" + name + ".dll";
        if (!MoveFileA(tmpPath.c_str(), finalPath.c_str())) {
            std::cout << "[GameManager] Reload: no .tmp artifact for " << name
                      << " (falling back to existing DLL)" << std::endl;
        }
    }

    // 加载新 DLL 并重新注册
    std::cout << "[GameManager] Hot-reloading plugin: " << name << std::endl;
    return LoadPlugin(name);
#else
    (void)name;
    return false;
#endif
}

IGameModule* GameManager::Activate(const std::string& name) {
    auto it = m_factories.find(name);
    if (it == m_factories.end()) {
        // 未注册: 尝试按名加载独立插件 DLL(游戏核心与引擎本体解耦)
        if (!LoadPlugin(name)) {
            std::cerr << "[GameManager] Game module not found: " << name << std::endl;
            return nullptr;
        }
        it = m_factories.find(name);
        if (it == m_factories.end()) {
            std::cerr << "[GameManager] Plugin did not register module: " << name << std::endl;
            return nullptr;
        }
    }
    if (m_current) {
        if (m_current->GetName() == name) return m_current; // 已激活
        Deactivate();
    }
    m_current = it->second();
    std::cout << "[GameManager] Activated game module: " << name << std::endl;
    return m_current;
}

void GameManager::Deactivate() {
    // 单例游戏模块不销毁;仅解除当前引用(工厂返回单例,重复 Activate 复用)
    m_current = nullptr;
}

} // namespace Game
