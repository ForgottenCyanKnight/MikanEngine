#pragma once
// GameManager.h - 游戏模块管理器(引擎与游戏内容的解耦入口)
// 游戏实现 IGameModule 并静态注册工厂;引擎启动/切换游戏时 Create(name) 激活。
// 引擎所有对游戏的调用都经 GetCurrent()(可为 nullptr = 无活动游戏,纯编辑器)。
#include "Platform/Export.h"
#include "Game/IGameModule.h"

#include <string>
#include <unordered_map>
#include <functional>

namespace Game {

using GameFactory = std::function<IGameModule*()>;

class MIKAN_API GameManager {
public:
    static GameManager& GetInstance();

    // 注册游戏工厂(游戏模块静态初始化时调用;name 与 --game 参数一致)
    void Register(const std::string& name, GameFactory factory);

    // 按名创建并激活为当前游戏(重复创建返回已激活实例)。
    // 未注册时自动尝试加载独立插件 DLL(见 LoadPlugin)。未找到返回 nullptr。
    IGameModule* Activate(const std::string& name);
    // 停用当前游戏(不销毁;下次 Activate 可复用)
    void Deactivate();

    // 加载独立游戏插件 DLL(与 Editor.dll 同模式): 约定导出两个 C 函数
    //   const char* GetGameModuleName();
    //   Game::IGameModule* CreateGameModule();
    // 候选 DLL 名: <name>.dll 或 Game<name>.dll(exe 目录)。已注册则直接返回 true。
    bool LoadPlugin(const std::string& name);

    // 重新加载插件 DLL(编译新版本后热更新,不重启引擎):
    // 卸载旧 DLL → 加载新 DLL → 重新注册;若当前激活游戏是该插件,则重新激活并通知 OnSceneLoaded。
    // 注意: 须在游戏非运行态调用(运行中卸载 DLL 会崩溃)。
    bool ReloadPlugin(const std::string& name);

    IGameModule* GetCurrent() const { return m_current; }
    bool HasCurrent() const { return m_current != nullptr; }

private:
    GameManager() = default;
    ~GameManager() = default;
    GameManager(const GameManager&) = delete;
    GameManager& operator=(const GameManager&) = delete;

    std::unordered_map<std::string, GameFactory> m_factories;
    IGameModule* m_current = nullptr;
    std::unordered_map<std::string, void*> m_pluginHandles; // 插件 DLL 句柄(Windows HMODULE),供重载
};

} // namespace Game
