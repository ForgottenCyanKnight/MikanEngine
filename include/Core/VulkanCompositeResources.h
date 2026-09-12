#pragma once

#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"

void VulkanComposite_CreateRenderPass(ImGui_ImplVulkanH_Window* wd);
void VulkanComposite_CreateFramebuffers(ImGui_ImplVulkanH_Window* wd);
void DestroyCompositeResources();
