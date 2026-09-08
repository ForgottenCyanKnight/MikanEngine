#pragma once
// GameContact2D.h - 2D 接触/触发事件演示(IGameModule 插件)
// 场景 scenes/contact2d.json: 组件路径物理(rigidbody2d + collider2d),
// 动态 Box 受重力下落: 先穿过 Sensor(静态触发器, onEnter/onExit), 再落到 Ground(碰撞 onEnter)。
// 演示 Contact2DComponent 用法: 场景加载后给实体挂组件并绑定回调, 回调在播放态物理结算后触发。
#include "Platform/Export.h"
#include "Game/IGameModule.h"
#include "ECS/Types.h"
#include <SDL3/SDL.h>

class Renderer2D;

namespace Game {

// 注意: 具体游戏类不加 MIKAN_API(否则构造/析构走 dllimport 找不到符号);
// 基类 IGameModule 的导出符号(虚表)来自 Game.dll, 链接 Game.lib 即可。
class GameContact2D : public IGameModule {
public:
    static GameContact2D& GetInstance();

    const char* GetName() const override { return "contact2d"; }
    void OnSceneLoaded() override;
    void OnUpdate(float deltaTime) override;        // 物理自动; 此处仅定时重播音效(序6 验证)
    void OnGameStop() override {}                    // 演示场景, 停止无需重置
    void OnKey(SDL_Keycode key) override;            // N 键: 场景切换演示(游戏事件触发)
    void OnRenderUI(Renderer2D&, int, int) override {}

private:
    GameContact2D() = default;
    ~GameContact2D() = default;
    GameContact2D(const GameContact2D&) = delete;
    GameContact2D& operator=(const GameContact2D&) = delete;

    ECS::Entity m_box = ECS::INVALID_ENTITY;    // "Box"(动态, 碰撞 + 穿过传感器)
    ECS::Entity m_sensor = ECS::INVALID_ENTITY; // "Sensor"(悬浮触发器)
    float m_audioTimer = 0.0f;                  // 定时重播音效验证(序6)
    float m_animTimer = 0.0f;                   // 定时打印动画帧验证(序2)
};

} // namespace Game
