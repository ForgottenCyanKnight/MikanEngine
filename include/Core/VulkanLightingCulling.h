#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

VkBuffer UpdatePointLightBuffer();

VkBuffer GetSceneClusterGridBuffer();
VkBuffer GetGameClusterGridBuffer();

void DispatchSceneClusterCull(VkCommandBuffer commandBuffer,
                              const glm::mat4& view,
                              const glm::mat4& proj,
                              float screenW,
                              float screenH);

void DispatchGameClusterCull(VkCommandBuffer commandBuffer,
                             const glm::mat4& view,
                             const glm::mat4& proj,
                             float screenW,
                             float screenH);

void RenderPointShadowMaps(VkCommandBuffer commandBuffer);

