#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

// Runtime-owned render resources that are used by the frame pipeline but do
// not belong to the frame recording/lifecycle coordinator itself.
VkBuffer GetShIrradianceBuffer();

void RenderParticlePass(
    VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
    VkRenderPass renderPass, uint32_t subpass, const glm::mat4& view,
    const glm::mat4& proj, const glm::vec3& cameraPosition,
    const glm::vec2& taaJitter);

// Swapchain recreation only needs to release renderer pipeline variants. Full
// Vulkan shutdown additionally releases the simulation-owned GPU resources.
void CleanupParticleRenderers();
void CleanupParticleResources();
