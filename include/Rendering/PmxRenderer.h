#pragma once
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstddef>
class ModelRenderer;
class SceneRenderer;

// Dedicated PMX material submission before the shared fullscreen PBR lighting.
MIKAN_API void RenderPmxMaterials(SceneRenderer& scene, VkCommandBuffer commands,
    VkRenderPass renderPass, uint32_t width, uint32_t height,
    const glm::mat4& view, const glm::mat4& projection,
    const glm::vec3& lightDirection, const glm::vec3& lightColor,
    const glm::vec2& jitter, uint32_t viewId = 0, int shadowSlot = 0);

// Forward edges/additive sphere highlights after shared PBR scene lighting.
MIKAN_API void RenderPmxScene(SceneRenderer& scene, VkCommandBuffer commands,
    VkRenderPass renderPass, uint32_t width, uint32_t height,
    const glm::mat4& view, const glm::mat4& projection,
    const glm::vec3& lightDirection, const glm::vec3& lightColor,
    const glm::vec2& jitter, uint32_t viewId = 0, int shadowSlot = 0);
void ReleasePmxRenderer(ModelRenderer* renderer);
// Zero means the PMX material does not cast; positive values are authored opacity.
float PmxShadowOpacity(ModelRenderer* renderer, size_t subMeshIndex);
