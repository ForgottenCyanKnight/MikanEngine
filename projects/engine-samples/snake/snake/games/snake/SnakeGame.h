#pragma once

// SnakeGame.h - 可直接复制到新项目的 2D 贪吃蛇模板。
// 场景树负责静态 UI 与图元，本模块只负责状态机、输入和动态实体同步。
#include "Game/IGameModule.h"
#include "ECS/Types.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

class Renderer2D;

namespace Game {

class SnakeGame final : public IGameModule {
public:
    static SnakeGame& GetInstance();

    const char* GetName() const override { return "snake"; }
    void OnSceneLoaded() override;
    void OnUpdate(float deltaTime) override;
    void OnKey(SDL_Keycode key) override;
    void OnRenderUI(Renderer2D& renderer, int viewWidth, int viewHeight) override;
    void OnGameStart() override;
    void OnGameStop() override;

private:
    SnakeGame() = default;
    ~SnakeGame() override = default;
    SnakeGame(const SnakeGame&) = delete;
    SnakeGame& operator=(const SnakeGame&) = delete;

    enum class State { Menu, Playing, GameOver };

    static constexpr int kGridWidth = 24;
    static constexpr int kGridHeight = 18;
    static constexpr float kCellSize = 32.0f;
    static constexpr float kTickInterval = 0.15f;

    State m_state = State::Menu;
    std::vector<glm::ivec2> m_snake;
    glm::ivec2 m_direction{1, 0};
    glm::ivec2 m_nextDirection{1, 0};
    glm::ivec2 m_food{10, 8};
    int m_score = 0;
    float m_tickAccumulator = 0.0f;

    bool m_sceneInitialized = false;
    ECS::Entity m_foodEntity = ECS::INVALID_ENTITY;
    std::vector<ECS::Entity> m_segmentEntities;
    ECS::Entity m_menuTitleEntity = ECS::INVALID_ENTITY;
    ECS::Entity m_startButtonEntity = ECS::INVALID_ENTITY;
    ECS::Entity m_retryButtonEntity = ECS::INVALID_ENTITY;
    ECS::Entity m_overlayEntity = ECS::INVALID_ENTITY;

    void InitializeSceneBindings();
    void EnsureSegment(std::size_t index);
    void SyncEntities();
    void UpdateVisibility();
    void StartNewGame();
    void ResetGame();
    void SpawnFood();
    void Step();
    bool HitsSelf(const glm::ivec2& cell) const;

    glm::vec2 GridToWorld(const glm::ivec2& cell) const {
        return glm::vec2(
            (static_cast<float>(cell.x) - kGridWidth * 0.5f) * kCellSize,
            (static_cast<float>(cell.y) - kGridHeight * 0.5f) * kCellSize);
    }
};

} // namespace Game
