#pragma once
// BreakoutGame.h - 打砖块色块原型（IGameModule 实现，独立插件 DLL，见本项目 games/breakout）
// 状态机: Menu → Playing → Win/Lose →(R 重开)
// 场景树资源(scenes/breakout.json): 相机/Canvas/菜单文本/背景/挡板/球;
// 砖块网格由本类启动时动态创建(色块实体),命中后销毁。
// 键盘: 左右/AD 移动挡板, 空格 发射/开始, R 重开。挡板移动用 SDL_GetKeyboardState 每帧读取。
// 注: 插件 DLL 内部类，不使用 MIKAN_API（同 SnakeGame，见 SnakeGame.h 注释）。
#include "Platform/Export.h"
#include "Game/IGameModule.h"
#include "ECS/Types.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

class Renderer2D;

namespace Game {

// 玩法常量(类外 inline constexpr:dllexport 类内 static constexpr 触发 C2487,同 Renderer2D 常量处理)
inline constexpr float kBOHalfW = 960.0f;   // 世界可视半宽(视口 1920)
inline constexpr float kBOHalfH = 540.0f;   // 世界可视半高(视口 1080)
inline constexpr float kBOPaddleW = 140.0f, kBOPaddleH = 18.0f;
inline constexpr float kBOKBallSize = 18.0f;
inline constexpr float kBOKBallSpeed = 420.0f;
inline constexpr float kBOKPaddleSpeed = 700.0f;
inline constexpr int kBOKBrickCols = 10, kBOKBrickRows = 6;
inline constexpr float kBOKBrickW = 60.0f, kBOKBrickH = 24.0f, kBOKBrickGap = 4.0f;

class BreakoutGame : public IGameModule {
public:
    static BreakoutGame& GetInstance();

    // ===== IGameModule =====
    const char* GetName() const override { return "breakout"; }
    void OnSceneLoaded() override;
    void OnUpdate(float deltaTime) override;
    void OnKey(SDL_Keycode key) override;
    void OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) override;
    void OnGameStop() override;                                 // 引擎停止: 重置回菜单,清理砖块实体

private:
    BreakoutGame() = default;
    ~BreakoutGame() = default;
    BreakoutGame(const BreakoutGame&) = delete;
    BreakoutGame& operator=(const BreakoutGame&) = delete;

    enum class State { Menu, Playing, Win, Lose };
    State m_state = State::Menu;

    // ===== 场景树实体 =====
    bool m_sceneInitialized = false;
    ECS::Entity m_paddleEntity = ECS::INVALID_ENTITY; // "Paddle"
    ECS::Entity m_ballEntity = ECS::INVALID_ENTITY;   // "Ball"
    ECS::Entity m_titleEntity = ECS::INVALID_ENTITY;  // "MenuTitle"
    ECS::Entity m_hintEntity = ECS::INVALID_ENTITY;   // "StartHint"
    glm::vec3 m_titleHomePos, m_hintHomePos;
    std::vector<ECS::Entity> m_brickEntities;         // "Brick0..N"(动态创建)

    // ===== 运行时状态 =====
    glm::vec2 m_ballPos{0.0f, 470.0f};
    glm::vec2 m_ballVel{0.0f, 0.0f};
    float m_paddleX = 0.0f;
    bool m_ballAttached = true; // 未发射,贴挡板
    int m_score = 0;

    void InitFromScene();
    void UpdateMenuVisibility();
    void StartNewGame();
    void ClearBricks();
    void CreateBricks();
    void StepBall(float deltaTime);
    void SyncEntities();
    bool BallHitsBrick(size_t index);
    glm::vec4 BrickColor(int row) const;
    void SyncEntity(ECS::Entity e, glm::vec2 pos, glm::vec2 size);
};

} // namespace Game
