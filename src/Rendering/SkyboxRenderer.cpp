#include "SkyboxRenderer.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "Core/Log.h"
#include "TexturePool.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <vector>
#include <cstring>
#include <functional>
#include <iostream>
SkyboxRenderer::SkyboxRenderer()
{
}

SkyboxRenderer::~SkyboxRenderer()
{
    std::cout << "[SkyboxRenderer] Destructor started" << std::endl;
    std::cout << "[SkyboxRenderer] Calling Cleanup..." << std::endl;
    Cleanup();
    std::cout << "[SkyboxRenderer] Destructor completed" << std::endl;
}

void SkyboxRenderer::Cleanup()
{
    extern VkDevice g_Device;
    extern VkAllocationCallbacks* g_Allocator;
    bool deviceValid = (g_Device != VK_NULL_HANDLE);
    
    if (m_VertexBuffer != VK_NULL_HANDLE && deviceValid) {
        vkDestroyBuffer(g_Device, m_VertexBuffer, g_Allocator);
        m_VertexBuffer = VK_NULL_HANDLE;
    }
    if (m_VertexBufferMemory != VK_NULL_HANDLE && deviceValid) {
        vkFreeMemory(g_Device, m_VertexBufferMemory, g_Allocator);
        m_VertexBufferMemory = VK_NULL_HANDLE;
    }
    if (m_IndexBuffer != VK_NULL_HANDLE && deviceValid) {
        vkDestroyBuffer(g_Device, m_IndexBuffer, g_Allocator);
        m_IndexBuffer = VK_NULL_HANDLE;
    }
    if (m_IndexBufferMemory != VK_NULL_HANDLE && deviceValid) {
        vkFreeMemory(g_Device, m_IndexBufferMemory, g_Allocator);
        m_IndexBufferMemory = VK_NULL_HANDLE;
    }

    // 不清理纹理池，因为它是共享的（由 EditorManager 管理）
    m_TexturePool = nullptr;
    
    BaseRenderer::Cleanup();
}

bool SkyboxRenderer::CreateVertexBuffer()
{
    std::vector<float> vertices = {
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
    };

    VkDeviceSize bufferSize = sizeof(float) * vertices.size();

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult err = vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_VertexBuffer);
    if (err != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_VertexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits, 
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_VertexBufferMemory);
    if (err != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(g_Device, m_VertexBuffer, m_VertexBufferMemory, 0);

    void* data;
    vkMapMemory(g_Device, m_VertexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices.data(), (size_t)bufferSize);
    vkUnmapMemory(g_Device, m_VertexBufferMemory);

    return true;
}

bool SkyboxRenderer::CreateIndexBuffer()
{
    std::vector<uint32_t> indices = {
        0, 1, 2, 2, 3, 0,
        1, 5, 6, 6, 2, 1,
        7, 6, 5, 5, 4, 7,
        4, 0, 3, 3, 7, 4,
        4, 5, 1, 1, 0, 4,
        3, 2, 6, 6, 7, 3
    };

    m_IndexCount = static_cast<uint32_t>(indices.size());
    VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult err = vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_IndexBuffer);
    if (err != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_IndexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits, 
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_IndexBufferMemory);
    if (err != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(g_Device, m_IndexBuffer, m_IndexBufferMemory, 0);

    void* data;
    vkMapMemory(g_Device, m_IndexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, indices.data(), (size_t)bufferSize);
    vkUnmapMemory(g_Device, m_IndexBufferMemory);

    return true;
}

void SkyboxRenderer::Init(VkRenderPass renderPass)
{
    // 使用 EditorManager 的共享纹理池，避免分辨率改变时纹理资源被销毁
    m_TexturePool = g_TexturePool;
    if (m_TexturePool == nullptr) {
        fprintf(stderr, "SkyboxRenderer: TexturePool is null\n");
        return;
    }

    if (!CreateVertexBuffer()) {
        fprintf(stderr, "Failed to create vertex buffer\n");
        return;
    }

    if (!CreateIndexBuffer()) {
        fprintf(stderr, "Failed to create index buffer\n");
        return;
    }

    m_Descriptor.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    
    if (!m_Descriptor.CreateLayout()) {
        fprintf(stderr, "Failed to create descriptor set layout\n");
        return;
    }
    
    if (!m_Descriptor.CreatePool(1)) {
        fprintf(stderr, "Failed to create descriptor pool\n");
        return;
    }

    // Default skybox is an engine asset (not project data).
    // Optional engine resources (2026-08): skybox/ & bluenoise are not consumed by the
    // physical-sky pipeline (fullscreen.frag samples skyRT only) - load them only when
    // present so trimmed release packages run without spurious ERR logs.
    std::string skyboxPath = EngineConfig::GetEngineTexturePath("skybox");
#ifdef __ANDROID__
    // Android：APK assets 不是真实文件系统，std::filesystem 读不到；用 SDL_GetPathInfo（SDL 对相对路径 fallback 到 assets://）
    SDL_PathInfo pinfo{};
    if (SDL_GetPathInfo(skyboxPath.c_str(), &pinfo) && pinfo.type == SDL_PATHTYPE_DIRECTORY) {
        m_TexturePool->LoadCubemapFromFaces("skybox", skyboxPath);
    } else {
        fprintf(stderr, "[SkyboxRenderer] skybox/ not present - cubemap skybox disabled (optional engine asset)\n");
    }
    if (SDL_GetPathInfo(EngineConfig::GetEngineTexturePath("bluenoise.png").c_str(), &pinfo) && pinfo.type == SDL_PATHTYPE_FILE) {
        m_TexturePool->LoadTexture2D("bluenoise", EngineConfig::GetEngineTexturePath("bluenoise.png"), SamplerType::NearestRepeat);
    }
#else
    std::error_code ec;
    if (std::filesystem::is_directory(skyboxPath, ec)) {
        m_TexturePool->LoadCubemapFromFaces("skybox", skyboxPath);
    } else {
        fprintf(stderr, "[SkyboxRenderer] skybox/ not present - cubemap skybox disabled (optional engine asset)\n");
    }

    std::string blueNoisePath = EngineConfig::GetEngineTexturePath("bluenoise.png");
    if (std::filesystem::exists(blueNoisePath, ec)) {
        m_TexturePool->LoadTexture2D("bluenoise", blueNoisePath, SamplerType::NearestRepeat);
    }
#endif

    if (!CreateDescriptorSet()) {
        fprintf(stderr, "Failed to create descriptor set\n");
        return;
    }

    m_TextureName = "skybox";
    UpdateDescriptorSet();

    PipelineConfig config;
    config.vertShader = "skybox.vert.spv";
    config.fragShader = "skybox.frag.spv";
    config.cullMode = VK_CULL_MODE_FRONT_BIT;
    config.depthTest = true;
    config.depthWrite = false;
    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    config.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    config.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        0,
        0,
        0,
        0
    };
    config.subpass = 1;               // MRT 几何 subpass（0=z-prepass）
    
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; // tint 在 fragment 读
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(SkyboxUniformData);
    
    VkVertexInputBindingDescription bindingDesc = {};
    bindingDesc.binding = 0;
    bindingDesc.stride = 3 * sizeof(float);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    config.vertexBindings.push_back(bindingDesc);
    
    VkVertexInputAttributeDescription attrDesc = {};
    attrDesc.binding = 0;
    attrDesc.location = 0;
    attrDesc.format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc.offset = 0;
    config.vertexAttributes.push_back(attrDesc);

    if (!m_Pipeline.Create(renderPass, m_Descriptor.GetLayout(), config)) {
        fprintf(stderr, "Failed to create pipeline\n");
        return;
    }
}

void SkyboxRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
    if (!m_Enabled) {
        return;
    }

    if (m_Pipeline.GetPipeline() == VK_NULL_HANDLE) {
        return;
    }

    if (m_DescriptorSet == VK_NULL_HANDLE) {
        fprintf(stderr, "[SkyboxRenderer] Descriptor set is null\n");
        return;
    }

    SkyboxUniformData pc = {};
    pc.view = glm::mat4(glm::mat3(view));
    pc.proj = proj;
    pc.tintAndIntensity = glm::vec4(m_Tint, m_Intensity);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline.GetPipeline());

    vkCmdPushConstants(commandBuffer, m_Pipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SkyboxUniformData), &pc);

    VkBuffer vertexBuffers[] = { m_VertexBuffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindIndexBuffer(commandBuffer, m_IndexBuffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, 
        m_Pipeline.GetLayout(), 0, 1, &m_DescriptorSet, 0, nullptr);

    vkCmdDrawIndexed(commandBuffer, m_IndexCount, 1, 0, 0, 0);
}

void SkyboxRenderer::UpdateDescriptorSet()
{
    if (m_DescriptorSet == VK_NULL_HANDLE || m_TexturePool == nullptr) return;
    const TextureInfo* tex = m_TexturePool->GetTexture(m_TextureName);
    if (!tex) {
        fprintf(stderr, "[SkyboxRenderer] UpdateDescriptorSet: '%s' not found\n", m_TextureName.c_str());
        return;
    }

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = tex->imageView;
    imageInfo.sampler = m_TexturePool->GetSampler(m_TextureName);

    VkWriteDescriptorSet descriptorWrite = {};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = m_DescriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(g_Device, 1, &descriptorWrite, 0, nullptr);
}

void SkyboxRenderer::SetTexture(const std::string& textureName)
{
    if (textureName == m_TextureName) return;
    if (m_TexturePool == nullptr) return;

    const TextureInfo* tex = m_TexturePool->GetTexture(textureName);
    if (!tex) {
        fprintf(stderr, "[SkyboxRenderer] SetTexture: '%s' not found in TexturePool, keeping '%s'\n",
            textureName.c_str(), m_TextureName.c_str());
        return;
    }

    m_TextureName = textureName;
    UpdateDescriptorSet();
}

void SkyboxRenderer::SyncFromScene()
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    // 按场景树顺序递归找第一个带 SkyboxComponent 的实体
    ECS::Entity found = ECS::INVALID_ENTITY;
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (found != ECS::INVALID_ENTITY) return;
        if (coordinator.HasComponent<ECS::SkyboxComponent>(e)) { found = e; return; }
        for (const auto& child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit(root);

    if (found == ECS::INVALID_ENTITY) {
        // 场景中无 SkyboxComponent：不渲染天空盒,显示渲染目标清屏颜色
        SetEnabled(false);
        return;
    }

    auto& sb = coordinator.GetComponent<ECS::SkyboxComponent>(found);
    // 诊断（临时）：skybox 状态变化打印
    static bool s_loggedSkybox = false;
    if (!s_loggedSkybox || sb.enabled != s_loggedSkybox) {
        printf("[SkyboxRenderer] scene skybox enabled=%d texture=%s\n", (int)sb.enabled, sb.textureName.c_str());
        s_loggedSkybox = sb.enabled;
    }
    SetEnabled(sb.enabled);
    SetTint(sb.tint);
    SetIntensity(sb.intensity);
    SetTexture(sb.textureName);
}
