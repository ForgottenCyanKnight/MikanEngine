#pragma once
// SnakeGame.h - 贪吃蛇玩法原型(2D 玩法生命周期示例,实现 IGameModule 注册到 GameManager)
// 状态机: Menu → Playing → GameOver →(R 重开)
// 资源场景树化: 蛇身/食物/背景/菜单 UI 均定义为场景树实体(见 assets/snake.json),
// 本类只负责动态逻辑(移动/碰撞/增长)并每帧同步实体位置;渲染由 Canvas2D 场景树承载。
// 引擎经 GameManager 调用(OnSceneLoaded/OnUpdate/OnKey/OnRenderUI),不直接引用本类。
#include "Platform/Export.h"
#include "Game/IGameModule.h"
#include "ECS/Types.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

class Renderer2D;

namespace Game {

// 注: SnakeGame 是独立插件 DLL(见 games/snake),不使用 MIKAN_API(dllexport/dllimport)——
// 若带 dllimport,插件内自己定义的成员(构造/内联函数)会被误标为从 Game.dll 导入。
class SnakeGame : public IGameModule {
public:
    static SnakeGame& GetInstance();

    // ===== IGameModule =====
    const char* GetName() const override { return "snake"; }
    void OnSceneLoaded() override;                              // 场景加载后:缓存实体 + 绑定按钮
    void OnUpdate(float deltaTime) override;                    // 游戏逻辑(tick 移动/碰撞 + 实体同步)
    void OnKey(SDL_Keycode key) override;                       // 键盘事件
    void OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) override; // UI 层: 分数/结束提示(菜单由场景树画)
    void OnGameStop() override;                                 // 引擎停止: 重置回菜单,清理运行时实体

private:
    SnakeGame() = default;
    ~SnakeGame() = default;
    SnakeGame(const SnakeGame&) = delete;
    SnakeGame& operator=(const SnakeGame&) = delete;

    enum class State { Menu, Playing, GameOver };
    State m_state = State::Menu;

    static constexpr int kGridW = 24;   // 网格宽(格)
    static constexpr int kGridH = 18;   // 网格高(格)
    static constexpr float kCell = 32.0f; // 格尺寸(世界像素)
    static constexpr float kTickInterval = 0.15f; // 每步间隔(秒)

    std::vector<glm::ivec2> m_snake; // 蛇身(0=头), 格子坐标
    glm::ivec2 m_dir{1, 0};          // 当前移动方向
    glm::ivec2 m_nextDir{1, 0};      // 待生效方向(下一 tick)
    glm::ivec2 m_food{10, 8};
    int m_score = 0;
    float m_tickAccum = 0.0f;

    // ===== 场景树实体(assets/snake.json 定义;动态逻辑每帧同步)=====
    bool m_sceneInitialized = false;
    ECS::Entity m_foodEntity = ECS::INVALID_ENTITY;      // "Food"
    std::vector<ECS::Entity> m_snakeEntities;            // "Snake0..N"(增长时动态创建)
    ECS::Entity m_titleEntity = ECS::INVALID_ENTITY;     // "MenuTitle"(菜单显隐)
    ECS::Entity m_buttonEntity = ECS::INVALID_ENTITY;    // "StartButton"(onClick 绑定)
    glm::vec3 m_titleHomePos, m_buttonHomePos;           // 菜单实体原始位置(隐藏时移出屏幕)
    ECS::Entity m_dirBtn[4] = { ECS::INVALID_ENTITY, ECS::INVALID_ENTITY, ECS::INVALID_ENTITY, ECS::INVALID_ENTITY }; // 方向键(移动端虚拟按键)
    glm::vec3 m_dirBtnHome[4];
    ECS::Entity m_retryEntity = ECS::INVALID_ENTITY;   // "RetryButton"(GameOver 显示)
    glm::vec3 m_retryHomePos;
    ECS::Entity m_dimEntity = ECS::INVALID_ENTITY;     // "OverlayDim"(GameOver 暗影, 场景树实体保证与按钮同周期排序)
    glm::vec3 m_dimHomePos;

    void InitFromScene();          // 按名缓存实体 + 绑定开始按钮 onClick
    void EnsureSnakeEntity(size_t index); // 蛇身实体不足时创建(吃食物增长)
    void SyncEntities();           // 每帧: 同步蛇身/食物实体位置
    void UpdateMenuVisibility();   // 状态切换: 菜单 UI 实体移入/移出屏幕

    void StartNewGame();
    void Reset();
    void SpawnFood();
    void Step();                     // 单步移动 + 碰撞检测
    bool HitSelf(const glm::ivec2& head) const;

    // 格子 → 世界像素(网格中心对齐世界原点)
    // 注意: 画布坐标 y 向下(顶部=0) —— 本函数返回的 y 即画布 y(row 0 在顶部)
    glm::vec2 GridToWorld(const glm::ivec2& c) const {
        return glm::vec2((c.x - kGridW * 0.5f) * kCell, (c.y - kGridH * 0.5f) * kCell);
    }
};

} // namespace Game
