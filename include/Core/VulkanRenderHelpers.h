#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "Rendering/PostProcessChain.h"

class CascadeShadowRenderer;
struct RenderWorld;

void CompositeToFinalBarrier(VkCommandBuffer commandBuffer, VkImage compositeImage);

void RenderUIOverlay(VkCommandBuffer commandBuffer,
                     uint32_t width,
                     uint32_t height,
                     VkRenderPass uiPass,
                     VkFramebuffer fb,
                     bool swapchainMode,
                     const glm::mat4* gridView = nullptr,
                     const glm::mat4* gridProj = nullptr,
                     const glm::mat4* renderView = nullptr,
                     const glm::mat4* renderProj = nullptr);

bool GetSceneDirectionalLight(const RenderWorld& world,
                               glm::vec3& dir,
                               glm::vec3& color,
                               float& intensity);

void FillCsmIntoUExt(PostProcessChain::ExternalInputs& ext,
                     CascadeShadowRenderer* csm,
                     int slot,
                     VkSampler shadowSampler);

void FillAtmosphereTransmittanceIntoExt(PostProcessChain::ExternalInputs& ext);

void FillCloudSettings(const RenderWorld& world,
                       PostProcessQuad::CameraUBO& ubo);

