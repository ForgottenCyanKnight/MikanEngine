// Vulkan runtime render resources shared by the frame pipeline.

#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanRuntimeResources.h"

#include "Core/EngineConfig.h"
#include "Core/EngineGlobal.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanFrameLoop.h"
#include "Core/VulkanManager.h"
#include "AtmosphereRenderer.h"
#include "Game/GameManager.h"
#include "Rendering/CpuClothRenderer.h"
#include "Rendering/CpuClothSimulation.h"
#include "Rendering/GpuSphContainerRenderer.h"
#include "Rendering/GpuSphSimulation.h"
#include "Rendering/ParticleRenderer.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/TexturePool.h"

#include <cstring>

extern AtmosphereRenderer g_AtmosphereRenderer;

namespace {

ParticleRenderer s_particleRenderer;
CpuClothRenderer s_cpuClothRenderer;
GpuSphContainerRenderer s_gpuSphContainerRenderer;

bool IsCpuClothProjectActive()
{
    auto* game = Game::GameManager::GetInstance().GetCurrent();
    return game != nullptr && game->GetName() != nullptr &&
           std::strcmp(game->GetName(), "clothspring") == 0;
}

} // namespace

void RenderParticlePass(
    VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
    VkRenderPass renderPass, uint32_t subpass, const glm::mat4& view,
    const glm::mat4& proj, const glm::vec3& cameraPosition,
    const glm::vec2& taaJitter)
{
    GpuSphSimulation& gpuSph = GpuSphSimulation::GetInstance();
    if (IsGpuSphProjectActive() && gpuSph.IsInitialized() &&
        gpuSph.GetRenderBuffer() != VK_NULL_HANDLE) {
        s_gpuSphContainerRenderer.Render(
            commandBuffer, width, height, renderPass, subpass, view, proj,
            gpuSph.GetContainerCenter(), gpuSph.GetContainerHalfExtents(),
            gpuSph.GetContainerRotation());
        s_particleRenderer.RenderGpuBuffer(
            commandBuffer, width, height, renderPass, subpass, view, proj,
            cameraPosition, taaJitter, gpuSph.GetRenderBuffer(),
            gpuSph.GetParticleCount());
        return;
    }

    if (IsCpuClothProjectActive()) {
        s_cpuClothRenderer.Render(
            commandBuffer, width, height, renderPass, subpass, view, proj,
            CpuClothSimulation::GetInstance());
    }

    s_particleRenderer.Render(commandBuffer, width, height, renderPass, subpass,
                              view, proj, cameraPosition, taaJitter,
                              &g_SceneRenderer.GetRenderWorld().particles);
}

void CleanupParticleRenderers()
{
    s_particleRenderer.Cleanup();
    s_cpuClothRenderer.Cleanup();
    s_gpuSphContainerRenderer.Cleanup();
}

void CleanupParticleResources()
{
    CleanupParticleRenderers();
    GpuSphSimulation::GetInstance().Cleanup();
}

VkBuffer GetShIrradianceBuffer()
{
    if (g_AtmosphereRenderer.IsInitialized()) {
        VkBuffer atmoSh = g_AtmosphereRenderer.GetSkyCubeSHBuffer();
        if (atmoSh) return atmoSh;
    }
    static VkBuffer buf = VK_NULL_HANDLE;
    static VkDeviceMemory mem = VK_NULL_HANDLE;
    if (buf != VK_NULL_HANDLE) return buf;

    float sh[27] = {0};
    if (g_TexturePool) g_TexturePool->ProjectSHIrradiance("sky_hdr", sh);

    VkBufferCreateInfo binfo = {};
    binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    binfo.size = 144;
    binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &binfo, g_Allocator, &buf) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &props);
    uint32_t mt = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((props.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mt = i;
            break;
        }
    }
    if (mt == VK_MAX_MEMORY_TYPES) return VK_NULL_HANDLE;

    VkMemoryAllocateInfo ainfo = {};
    ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &ainfo, g_Allocator, &mem) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    vkBindBufferMemory(g_Device, buf, mem, 0);

    void* data = nullptr;
    if (vkMapMemory(g_Device, mem, 0, 144, 0, &data) == VK_SUCCESS) {
        // The shader uses vec4 shCoefs[9] (std140, 16 bytes per element), so
        // the CPU-side RGB coefficients need one padding float per element.
        float shPacked[36] = {0};
        for (int i = 0; i < 9; i++) {
            shPacked[i * 4 + 0] = sh[i * 3 + 0];
            shPacked[i * 4 + 1] = sh[i * 3 + 1];
            shPacked[i * 4 + 2] = sh[i * 3 + 2];
        }
        std::memcpy(data, shPacked, 144);
        vkUnmapMemory(g_Device, mem);
    }
    return buf;
}
