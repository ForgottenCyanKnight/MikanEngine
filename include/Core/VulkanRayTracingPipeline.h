#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "Rendering/RayTracing/RayTracingEnvironment.h"
class PostProcessChain;
// Record once before either viewport; generation is shared and anchored to the main camera.
const RayTracingEnvironment& PrepareHardwareRayTracingEnvironment(VkCommandBuffer cmd,const glm::vec3& fallbackCamera);
// Returns true when the chain requests a pure RT world, including unsupported-device handling.
bool RenderPureRayTracingView(VkCommandBuffer cmd,PostProcessChain& chain,
    uint32_t workingWidth,uint32_t workingHeight,uint32_t outputWidth,uint32_t outputHeight,
    VkRenderPass finalPass,VkFramebuffer finalFramebuffer,VkRenderPass uiPass,
    const glm::mat4& view,const glm::mat4& proj,int viewSlot,bool swapchain);
