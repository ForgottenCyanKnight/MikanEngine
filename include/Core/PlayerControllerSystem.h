#pragma once

#include "Platform/Export.h"
#include "ECS/System.h"
#include "ECS/Components.h"

#include <glm/vec2.hpp>
#include <unordered_map>

namespace ECS {

// 原型期的第三人称玩家控制器：输入只改变动态刚体的水平速度，
// 重力、地形接触和 Transform 回写仍由 PhysicsSystem 统一负责。
class MIKAN_API PlayerControllerSystem final : public System {
public:
    void Update(float deltaTime) override;
    void OnEntityRemoved(Entity entity) override;

    // Deterministic input override used by the renderer-independent gameplay
    // smoke test. Normal desktop/mobile gameplay continues to use InputController.
    void SetSyntheticInput(const glm::vec2& move, bool jump);
    void ClearSyntheticInput();

private:
    struct RuntimeState {
        bool jumpWasDown = false;
        bool jumpArmed = false;
    };

    std::unordered_map<Entity, RuntimeState> m_Runtime;
    glm::vec2 m_SyntheticMove = glm::vec2(0.0f);
    bool m_SyntheticJump = false;
    bool m_UseSyntheticInput = false;
};

} // namespace ECS
