#pragma once

#include "ECS/Types.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <optional>

class SceneRenderer;
struct RenderWorld;

namespace Editor {

struct ScenePickHit {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    float distance = 0.0f;
    std::size_t subMeshIndex = static_cast<std::size_t>(-1);
};

struct ScenePickRay {
    glm::vec3 origin = glm::vec3(0.0f);
    glm::vec3 direction = glm::vec3(0.0f, 0.0f, -1.0f);
};

// SceneView uses the same CPU-side coordinate convention as the renderer:
// screen Y grows downward and the projection depth range is [-1, 1].
std::optional<ScenePickHit> PickSceneEntity(
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec2& mousePosition,
    const glm::vec2& viewportMin,
    const glm::vec2& viewportSize,
    const RenderWorld& renderWorld,
    SceneRenderer& sceneRenderer);

} // namespace Editor
