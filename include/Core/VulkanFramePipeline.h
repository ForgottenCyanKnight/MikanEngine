#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "Core/VulkanRuntimeResources.h"

#ifdef __ANDROID__
inline constexpr int kGameCsmSlot = 0;
#else
inline constexpr int kGameCsmSlot = 1;
#endif

extern glm::mat4 s_PrevView;
extern glm::mat4 s_PrevProj;
extern glm::mat4 s_PrevViewProj;
extern glm::mat4 s_PrevCloudViewProjScene;
extern glm::mat4 s_PrevCloudViewProjGame;
extern glm::vec3 s_PrevCloudWindOffsetScene;
extern glm::vec3 s_PrevCloudWindOffsetGame;
extern glm::vec3 s_PrevCloudHighWindOffsetScene;
extern glm::vec3 s_PrevCloudHighWindOffsetGame;
extern glm::vec2 g_PreviousTAAJitterGame;

void RenderSceneToTarget(const glm::mat4& view,
                          const glm::mat4& proj,
                          uint32_t frameIndex);

void RenderGameContent(VkCommandBuffer commandBuffer,
                       const glm::mat4& view,
                       const glm::mat4& proj,
                       bool usePhysicalSky);

void RenderGameToTarget(const glm::mat4& view,
                        const glm::mat4& proj,
                        const glm::vec3& cameraPos,
                        const glm::vec3& cameraFront,
                        const glm::vec3& cameraRight,
                        const glm::vec3& cameraUp,
                        uint32_t frameIndex);

void RenderGameComposite(const glm::mat4& view,
                         const glm::mat4& proj,
                         uint32_t frameIndex);
