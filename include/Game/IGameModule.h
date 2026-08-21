#pragma once
// IGameModule.h - 游戏模块接口(引擎与游戏内容的解耦边界)
// 引擎只认识本接口,不认识具体游戏(贪吃蛇等)。
// 游戏实现本接口并注册到 GameManager;引擎每帧经 GameManager 转发生命周期回调。
// 设计为 2D/3D 通用:OnRenderUI 画屏幕层内容;需要世界层绘制/3D 时在子类自行接 Canvas2D/Renderer2D。
#include "Platform/Export.h"
#include <SDL3/SDL.h>

class Renderer2D;

namespace Game {

class MIKAN_API IGameModule {
public:
    virtual ~IGameModule() = default;

    // 游戏标识(注册名,与 --game 参数/场景约定一致)
    virtual const char* GetName() const = 0;

    // 场景加载完成后回调(绑定场景树实体、初始化状态;场景重载后再次调用)
    virtual void OnSceneLoaded() = 0;

    // 每帧逻辑更新(deltaTime 秒)
    // 注意: 仅在"运行态"(工具栏播放且未暂停)时被引擎调用;暂停 = 引擎停止调用本方法
    virtual void OnUpdate(float deltaTime) = 0;

    // 始终执行回调(无论是否播放/暂停, 引擎每帧调用):
    // 用于"开始后即需响应"的逻辑(如物理原型玩家控制/相机跟随), 不被播放态 gate。
    virtual void OnAlwaysUpdate(float deltaTime) {}

    // 键盘事件(引擎事件循环转发)
    virtual void OnKey(SDL_Keycode key) = 0;

    // UI 层动态内容渲染(屏幕坐标,画布 y 向下;静态 UI 由场景树承载)
    virtual void OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) = 0;

    // ===== 引擎播放/暂停/停止生命周期(工具栏控制;默认空实现,游戏按需覆写)=====
    // 播放: 进入运行态(游戏可维持当前状态,如菜单等待玩家按键)
    virtual void OnGameStart() {}
    // 暂停: 引擎停止调用 OnUpdate(游戏如需暂停提示可覆写)
    virtual void OnGamePause() {}
    // 恢复: 引擎恢复调用 OnUpdate
    virtual void OnGameResume() {}
    // 停止: 退出运行态;游戏应重置到初始状态(如回到菜单,清理运行时实体)
    virtual void OnGameStop() {}
};

} // namespace Game
