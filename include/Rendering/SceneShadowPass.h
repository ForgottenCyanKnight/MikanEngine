#pragma once

#include "Platform/Export.h"
#include "Rendering/SceneRenderer.h"

class MIKAN_API SceneShadowPass final {
public:
    static void RenderPointShadowMaps(
        SceneRenderer& sceneRenderer,
        VkCommandBuffer commandBuffer,
        const SceneRenderer::ShadowLight* lights,
        int lightCount,
        int shadowMapSize);

    static void RenderCascadeShadowMaps(
        SceneRenderer& sceneRenderer,
        VkCommandBuffer commandBuffer,
        int slot,
        const glm::mat4& view,
        const glm::mat4& proj,
        const glm::vec3& lightDir);
};
