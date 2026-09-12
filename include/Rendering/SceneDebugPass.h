#pragma once

#include "Platform/Export.h"
#include "Rendering/SceneRenderer.h"

class MIKAN_API SceneDebugPass final {
public:
    static void Collect(SceneRenderer& sceneRenderer, RenderFrameContext& context);
    static void RenderOverlay(SceneRenderer& sceneRenderer,
                              VkCommandBuffer commandBuffer,
                              int width,
                              int height,
                              VkRenderPass uiPass,
                              const glm::mat4& view,
                              const glm::mat4& proj);
};
