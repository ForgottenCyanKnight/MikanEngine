// GameManager.cpp - 游戏模块管理器
#include "Game/GameManager.h"
#include "Core/LogStream.h"

#include <iostream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(MIKAN_OPENHARMONY)
#include "Core/ProjectManager.h"
#include <SDL3/SDL.h>
#endif

namespace Game {

GameManager& GameManager::GetInstance() {
    static GameManager instance;
    return instance;
}

void GameManager::Register(const std::string& name, GameFactory factory) {
    if (name.empty() || !factory) return;
    m_factories[name] = std::move(factory);
    LOGSTREAM(Info) << "[GameManager] Registered game module: " << name << std::endl;
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
        LOGSTREAM(Warn) << "[GameManager] LoadPlugin: no DLL for '" << name
                  << "' (tried " << candidates[0] << ", " << candidates[1] << ")" << std::endl;
        return false;
    }
    using GetNameFn = const char* (*)();
    using CreateFn  = IGameModule* (*)();
    auto getName = reinterpret_cast<GetNameFn>(GetProcAddress(h, "GetGameModuleName"));
    auto create  = reinterpret_cast<CreateFn>(GetProcAddress(h, "CreateGameModule"));
    if (!getName || !create) {
        LOGSTREAM(Error) << "[GameManager] LoadPlugin: " << loadedPath
                  << " missing GetGameModuleName/CreateGameModule exports" << std::endl;
        FreeLibrary(h);
        return false;
    }
    std::string pluginName = getName() ? getName() : name;
    Register(pluginName, [create]() -> IGameModule* { return create(); });
    m_pluginHandles[pluginName] = h; // 记录句柄(供 ReloadPlugin 卸载)
    LOGSTREAM(Info) << "[GameManager] Loaded game plugin: " << loadedPath << " -> '" << pluginName << "'" << std::endl;
    return true;
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(MIKAN_OPENHARMONY)
    const auto& manifest = ProjectManager::GetInstance().GetManifest();
    if (!manifest.valid || name != manifest.game) {
        LOGSTREAM(Warn) << "[GameManager] Linux gameplay module must match the selected project manifest: "
                        << name << std::endl;
        return false;
    }
    if (name == "." || name == ".." ||
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos) {
        LOGSTREAM(Error) << "[GameManager] Invalid Linux gameplay module name: " << name << std::endl;
        return false;
    }

    // A rejected library may already have registered script factories in its
    // static initializers. Retain that handle until normal project teardown
    // has removed callbacks, rather than unloading executable code immediately.
    if (m_pluginHandles.count(name)) return false;
    const char* executableDirectory = SDL_GetBasePath();
    if (!executableDirectory) {
        LOGSTREAM(Error) << "[GameManager] Could not locate the executable directory: "
                         << SDL_GetError() << std::endl;
        return false;
    }
    const std::string path = std::string(executableDirectory) + "libGame" + name + ".so";
    SDL_SharedObject* handle = SDL_LoadObject(path.c_str());
    if (!handle) {
        LOGSTREAM(Warn) << "[GameManager] Could not load " << path << ": " << SDL_GetError() << std::endl;
        return false;
    }
    m_pluginHandles.emplace(name, handle);

    using GetNameFn = const char* (*)();
    using CreateFn = IGameModule* (*)();
    const auto getName = reinterpret_cast<GetNameFn>(SDL_LoadFunction(handle, "GetGameModuleName"));
    const auto create = reinterpret_cast<CreateFn>(SDL_LoadFunction(handle, "CreateGameModule"));
    if (!getName || !create) {
        m_factories.erase(name);
        LOGSTREAM(Error) << "[GameManager] Missing GetGameModuleName/CreateGameModule exports: "
                         << path << std::endl;
        return false;
    }
    const char* declaredName = getName();
    if (!declaredName || name != declaredName) {
        m_factories.erase(name);
        LOGSTREAM(Error) << "[GameManager] Plugin name does not match project manifest: "
                         << path << std::endl;
        return false;
    }
    Register(name, [create]() { return create(); });
    LOGSTREAM(Info) << "[GameManager] Loaded game plugin: " << path << " -> '" << name << "'" << std::endl;
    return true;
#else
    (void)name;
    LOGSTREAM(Warn) << "[GameManager] LoadPlugin not supported on this platform" << std::endl;
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
            LOGSTREAM(Info) << "[GameManager] Reload: no .tmp artifact for " << name
                      << " (falling back to existing DLL)" << std::endl;
        }
    }

    // 加载新 DLL 并重新注册
    LOGSTREAM(Info) << "[GameManager] Hot-reloading plugin: " << name << std::endl;
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
            LOGSTREAM(Error) << "[GameManager] Game module not found: " << name << std::endl;
            return nullptr;
        }
        it = m_factories.find(name);
        if (it == m_factories.end()) {
            LOGSTREAM(Warn) << "[GameManager] Plugin did not register module: " << name << std::endl;
            return nullptr;
        }
    }
    if (m_current) {
        if (m_current->GetName() == name) return m_current; // 已激活
        Deactivate();
    }
    m_current = it->second();
    if (m_current) {
        LOGSTREAM(Info) << "[GameManager] Activated game module: " << name << std::endl;
    } else {
        LOGSTREAM(Error) << "[GameManager] Game factory returned no module: " << name << std::endl;
    }
    return m_current;
}

void GameManager::Deactivate() {
    // 单例游戏模块不销毁;仅解除当前引用(工厂返回单例,重复 Activate 复用)
    m_current = nullptr;
    m_worldPaused = false;
}

void GameManager::UnloadPlugins() {
    Deactivate();

#ifdef _WIN32
    for (const auto& [name, handle] : m_pluginHandles) {
        m_factories.erase(name);
        if (handle != nullptr) {
            FreeLibrary(static_cast<HMODULE>(handle));
        }
    }
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(MIKAN_OPENHARMONY)
    for (const auto& [name, handle] : m_pluginHandles) {
        m_factories.erase(name);
        SDL_UnloadObject(static_cast<SDL_SharedObject*>(handle));
    }
#else
    for (const auto& [name, handle] : m_pluginHandles) {
        (void)handle;
        m_factories.erase(name);
    }
#endif
    m_pluginHandles.clear();
}

} // namespace Game
