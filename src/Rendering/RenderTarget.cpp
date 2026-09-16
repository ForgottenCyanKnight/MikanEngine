#include "RenderTarget.h"
#include "EngineGlobal.h"
#include "Core/Log.h"
// 本文件 include 的三个 .inl 片段里有 LOGSTREAM 调用；.inl 自己不能插 include，
// 必须由包含者提供（见 RenderTargetDisplayResources.inl）。
#include "Core/LogStream.h"

// 注意：vulkan.h 为 Vulkan 1.4 头（VK_HEADER_VERSION 346）。input attachment 引用布局用
// VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL（1.4 核心化 KHR_maintenance5，替代已移除的 INPUT_ATTACHMENT_OPTIMAL）。

#include <stdio.h>
#include <iostream>

static bool CheckFormatSupport(VkPhysicalDevice physicalDevice, VkFormat format, VkImageTiling tiling, VkFormatFeatureFlags features) {
    VkFormatProperties formatProps;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &formatProps);
    
    if (tiling == VK_IMAGE_TILING_LINEAR && (formatProps.linearTilingFeatures & features) == features) {
        return true;
    } else if (tiling == VK_IMAGE_TILING_OPTIMAL && (formatProps.optimalTilingFeatures & features) == features) {
        return true;
    }
    return false;
}

static VkFormat SelectCompatibleFormat(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& preferredFormats, VkImageTiling tiling, VkFormatFeatureFlags features) {
    for (VkFormat format : preferredFormats) {
        if (CheckFormatSupport(physicalDevice, format, tiling, features)) {
            return format;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

static uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOGE("Failed to find suitable memory type!");
    return 0;
}

#include "RenderTargetLifecycle.inl"

#include "RenderTargetRenderPasses.inl"

#include "RenderTargetAttachmentResources.inl"

#include "RenderTargetDisplayResources.inl"

#include "RenderTargetRecording.inl"
#include "RenderTargetCompositeAndFinal.inl"
