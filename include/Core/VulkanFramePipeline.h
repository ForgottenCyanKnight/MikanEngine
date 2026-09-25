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

// Swapchain/window resize invalidates all temporal reprojection inputs. This
// resets the per-view camera history and jitter sequence without forcing the
// effects to run every frame afterwards.
void ResetFramePipelineTemporalState();

// 帧尾录制场景反射探针（cubemap）：本帧水面合成读到的是上一帧的捕获结果。
// 探针片元自带光照，不依赖本帧 IBL/后处理，因此放在帧尾不影响主视图关键路径。
void RenderSceneProbeCapture(VkCommandBuffer commandBuffer,
                             const glm::vec3& capturePosition,
                             bool enabled);

void RenderSceneToTarget(const glm::mat4& view,
                          const glm::mat4& proj,
                          uint32_t frameIndex);

void RenderGameContent(VkCommandBuffer commandBuffer,
                       const glm::mat4& view,
                       const glm::mat4& proj,
                       bool usePhysicalSky,
                       bool renderGameplayScene = true);

void RenderGameToTarget(const glm::mat4& view,
                        const glm::mat4& proj,
                        const glm::vec3& cameraPos,
                        const glm::vec3& cameraFront,
                        const glm::vec3& cameraRight,
                        const glm::vec3& cameraUp,
                        uint32_t frameIndex);

void RenderGameComposite(const glm::mat4& view,
                         const glm::mat4& proj,
                         uint32_t frameIndex,
                         bool renderGameplayScene = true);
