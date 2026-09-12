#pragma once

#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"

// Swapchain-only passes used while the engine is starting or switching projects.
// They deliberately avoid ECS, scene targets, post-processing, and lighting.
bool IsLoadingScreenActive();

void RenderLoadingScreenPass(VkCommandBuffer commandBuffer,
                             ImGui_ImplVulkanH_Window* wd,
                             ImGui_ImplVulkanH_Frame* frame);

void RenderProjectManagerPass(VkCommandBuffer commandBuffer,
                              ImGui_ImplVulkanH_Window* wd,
                              ImGui_ImplVulkanH_Frame* frame,
                              ImDrawData* drawData);
