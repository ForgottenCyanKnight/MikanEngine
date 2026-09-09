// WorldRenderer.cpp - Vulkan 体素世界渲染器
// 数据流: World (CPU FaceInstance) -> 视锥剔除 -> 每帧上传 mapped 实例缓冲 ->
//         vkCmdDraw(4, 面数) 一次绘制所有可见面（opaque+alphatest），水单独透明 pass
#include "World/WorldRenderer.h"
#include "EngineGlobal.h"
#include "Core/EngineConfig.h"

#include <SDL3_image/SDL_image.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <chrono>

namespace {
// 上传数据到 DEVICE_LOCAL 缓冲（staging + 一次性命令）
void CopyBuffer(VkBuffer dstBuffer, VkBuffer srcBuffer, VkDeviceSize size) {
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion = {};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);

    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
}
}

WorldRenderer::WorldRenderer() = default;
WorldRenderer::~WorldRenderer() { Cleanup(); }

void WorldRenderer::Init(VkRenderPass renderPass)
{
    if (m_Initialized) return;
    if (!CreateQuadBuffer()) {
        std::cout << "[WorldRenderer] FAILED: CreateQuadBuffer" << std::endl;
        printf("[WorldRenderer] Failed to create quad buffer");
        return;
    }
    if (!LoadAtlasTexture()) {
        std::cout << "[WorldRenderer] FAILED: LoadAtlasTexture" << std::endl;
        printf("[WorldRenderer] Failed to load atlas texture");
        // 图集失败时保持 m_Initialized=false（Render 有 guard 直接返回），
        // 不创建 descriptor/pipeline——否则绑定 NULL atlas view 会崩溃（0xC0000005）。
        return;
    }
    std::cout << "[WorldRenderer] Atlas loaded (" << m_AtlasWidth << "x" << m_AtlasHeight << ")" << std::endl;
    CreateDescriptorSet();
    CreatePipeline(renderPass);
    m_Initialized = true;
    printf("[WorldRenderer] Initialized");
}

void WorldRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
    // IRenderer 接口入口：从 view/proj 提取视锥，传入渲染相机位置
    if (!m_World || !m_Initialized) return;
    std::array<Plane, 6> frustum = AABBUtils::ExtractFrustumPlanes(proj * view);
    glm::vec3 cameraPos = glm::vec3(-view[3][0], -view[3][1], -view[3][2]);
    RenderWorld(commandBuffer, 0, 0, view, proj, cameraPos, frustum);
}

void WorldRenderer::RenderWorld(VkCommandBuffer commandBuffer, int width, int height,
                                const glm::mat4& view, const glm::mat4& proj,
                                const glm::vec3& cameraPos, const std::array<Plane, 6>& frustumPlanes)
{
    if (!m_World || !m_Initialized) return;
    if (commandBuffer == VK_NULL_HANDLE) return;

    // 回收到期的退役缓冲（延迟 2 帧销毁，确保 GPU 不再引用）
    RecycleRetiredBuffers();

    // 计时器（地形绘制分阶段耗时统计）
    auto tFrame = std::chrono::high_resolution_clock::now();

    // ---- 1. 收集可见面（World 内部锁保护，避免与网格 worker 线程竞争） ----
    auto tCollect = std::chrono::high_resolution_clock::now();
    m_World->CollectVisibleFaces(frustumPlanes, m_OpaqueFaces, m_AlphaFaces, m_TransparentFaces);

    if (m_OpaqueFaces.empty() && m_AlphaFaces.empty() && m_TransparentFaces.empty()) return;

    // ---- 2. 上传实例数据（动态扩容，host-visible mapped） ----
    auto tUpload = std::chrono::high_resolution_clock::now();
    EnsureInstanceCapacity(m_OpaqueFaces.size(), m_AlphaFaces.size(), m_TransparentFaces.size());
    if (!m_OpaqueFaces.empty() && m_InstanceMapped) {
        memcpy(m_InstanceMapped, m_OpaqueFaces.data(), m_OpaqueFaces.size() * sizeof(Chunk::FaceInstance));
    }
    if (!m_AlphaFaces.empty() && m_AlphaInstanceMapped) {
        memcpy(m_AlphaInstanceMapped, m_AlphaFaces.data(), m_AlphaFaces.size() * sizeof(Chunk::FaceInstance));
    }
    if (!m_TransparentFaces.empty() && m_TransparentInstanceMapped) {
        memcpy(m_TransparentInstanceMapped, m_TransparentFaces.data(), m_TransparentFaces.size() * sizeof(Chunk::FaceInstance));
    }

    // 上传完成 → 开始记录绘制（命令录制）耗时
    auto tDrawStart = std::chrono::high_resolution_clock::now();

    // ---- 3. 视口 ----
    if (width > 0 && height > 0) {
        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    // ---- 4. push constants ----
    WorldPushConstants pushConstants = {};
    pushConstants.projView = proj * view;
    pushConstants.cameraPos = glm::vec4(cameraPos, 1.0f);
    pushConstants.sunDir = glm::vec4(m_SunDir, 0.0f);

    // ---- 5. 绘制（三个独立缓冲，各自从 firstInstance=0 读取） ----
    VkDeviceSize offsets[] = { 0, 0 };

    if (!m_OpaqueFaces.empty() && m_InstanceBuffer != VK_NULL_HANDLE) {
        VkBuffer vbs[] = { m_QuadVertexBuffer, m_InstanceBuffer };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline.GetPipeline());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline.GetLayout(),
                                0, 1, &m_DescriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_Pipeline.GetLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(WorldPushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vbs, offsets);
        vkCmdDraw(commandBuffer, 4, static_cast<uint32_t>(m_OpaqueFaces.size()), 0, 0);
    }

    // 植物/树叶（双面，不剔除）
    if (!m_AlphaFaces.empty() && m_AlphaInstanceBuffer != VK_NULL_HANDLE) {
        VkBuffer vbs[] = { m_QuadVertexBuffer, m_AlphaInstanceBuffer };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_AlphaPipeline.GetPipeline());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_AlphaPipeline.GetLayout(),
                                0, 1, &m_DescriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_AlphaPipeline.GetLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(WorldPushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vbs, offsets);
        vkCmdDraw(commandBuffer, 4, static_cast<uint32_t>(m_AlphaFaces.size()), 0, 0);
    }

    // 水面（透明混合）
    if (!m_TransparentFaces.empty() && m_TransparentInstanceBuffer != VK_NULL_HANDLE) {
        VkBuffer vbs[] = { m_QuadVertexBuffer, m_TransparentInstanceBuffer };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TransparentPipeline.GetPipeline());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TransparentPipeline.GetLayout(),
                                0, 1, &m_DescriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_TransparentPipeline.GetLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(WorldPushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vbs, offsets);
        vkCmdDraw(commandBuffer, 4, static_cast<uint32_t>(m_TransparentFaces.size()), 0, 0);
    }

    // 地形绘制耗时统计（每 240 帧输出：分阶段平均 + 最近一次总耗时）
    {
        auto tEnd = std::chrono::high_resolution_clock::now();
        const double collectMs = std::chrono::duration<double, std::milli>(tUpload - tCollect).count();
        const double uploadMs = std::chrono::duration<double, std::milli>(tDrawStart - tUpload).count();
        const double drawMs = std::chrono::duration<double, std::milli>(tEnd - tDrawStart).count();
        const double totalMs = std::chrono::duration<double, std::milli>(tEnd - tFrame).count();

        static double s_accCollect = 0, s_accUpload = 0, s_accDraw = 0, s_accTotal = 0;
        static int s_fc = 0;
        s_accCollect += collectMs;
        s_accUpload += uploadMs;
        s_accDraw += drawMs;
        s_accTotal += totalMs;
        if (++s_fc >= 240) {
            // 暂时注释：地形绘制耗时打印
            // std::cout << "[WorldRenderer] 地形绘制耗时 avg240: collect=" << (s_accCollect / 240.0)
            //           << "ms upload=" << (s_accUpload / 240.0)
            //           << "ms draw=" << (s_accDraw / 240.0)
            //           << "ms total=" << (s_accTotal / 240.0) << "ms | last=" << totalMs
            //           << "ms faces=" << (m_OpaqueFaces.size() + m_AlphaFaces.size() + m_TransparentFaces.size())
            //           << std::endl;
            s_accCollect = s_accUpload = s_accDraw = s_accTotal = 0.0;
            s_fc = 0;
        }
    }
}

void WorldRenderer::EnsureInstanceCapacity(size_t opaqueCount, size_t alphaCount, size_t transparentCount)
{
    // 不透明缓冲
    VkDeviceSize required = opaqueCount * sizeof(Chunk::FaceInstance);
    if (required > 0 && (m_InstanceBuffer == VK_NULL_HANDLE || required > m_InstanceCapacity)) {
        VkDeviceSize newSize = (m_InstanceCapacity == 0) ? required * 2 : m_InstanceCapacity * 2;
        if (newSize < required) newSize = required;
        VkBuffer newBuffer = VK_NULL_HANDLE;
        VkDeviceMemory newMemory = VK_NULL_HANDLE;
        VkBufferCreateInfo bufInfo = {};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = newSize;
        bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &newBuffer) != VK_SUCCESS) return;
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(g_Device, newBuffer, &memReq);
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &newMemory) != VK_SUCCESS) {
            vkDestroyBuffer(g_Device, newBuffer, g_Allocator);
            return;
        }
        vkBindBufferMemory(g_Device, newBuffer, newMemory, 0);
        void* mapped = nullptr;
        vkMapMemory(g_Device, newMemory, 0, newSize, 0, &mapped);

        // 旧缓冲进退役队列（延迟 2 帧销毁，避免销毁正在录制/未提交的 draw 引用的缓冲）
        if (m_InstanceBuffer != VK_NULL_HANDLE) {
            m_RetiredBuffers.push_back({ m_InstanceBuffer, m_InstanceBufferMemory, m_InstanceMapped, 2 });
        }
        m_InstanceBuffer = newBuffer;
        m_InstanceBufferMemory = newMemory;
        m_InstanceMapped = mapped;
        m_InstanceCapacity = newSize;
    }

    // 植物/树叶缓冲
    VkDeviceSize alphaRequired = alphaCount * sizeof(Chunk::FaceInstance);
    if (alphaRequired > 0 && (m_AlphaInstanceBuffer == VK_NULL_HANDLE || alphaRequired > m_AlphaInstanceCapacity)) {
        VkDeviceSize newSize = (m_AlphaInstanceCapacity == 0) ? alphaRequired * 2 : m_AlphaInstanceCapacity * 2;
        if (newSize < alphaRequired) newSize = alphaRequired;
        VkBuffer newBuffer = VK_NULL_HANDLE;
        VkDeviceMemory newMemory = VK_NULL_HANDLE;
        VkBufferCreateInfo bufInfo = {};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = newSize;
        bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &newBuffer) != VK_SUCCESS) return;
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(g_Device, newBuffer, &memReq);
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &newMemory) != VK_SUCCESS) {
            vkDestroyBuffer(g_Device, newBuffer, g_Allocator);
            return;
        }
        vkBindBufferMemory(g_Device, newBuffer, newMemory, 0);
        void* mapped = nullptr;
        vkMapMemory(g_Device, newMemory, 0, newSize, 0, &mapped);

        if (m_AlphaInstanceBuffer != VK_NULL_HANDLE) {
            m_RetiredBuffers.push_back({ m_AlphaInstanceBuffer, m_AlphaInstanceBufferMemory, m_AlphaInstanceMapped, 2 });
        }
        m_AlphaInstanceBuffer = newBuffer;
        m_AlphaInstanceBufferMemory = newMemory;
        m_AlphaInstanceMapped = mapped;
        m_AlphaInstanceCapacity = newSize;
    }

    // 透明缓冲
    VkDeviceSize transparentRequired = transparentCount * sizeof(Chunk::FaceInstance);
    if (transparentRequired > 0 && (m_TransparentInstanceBuffer == VK_NULL_HANDLE || transparentRequired > m_TransparentInstanceCapacity)) {
        VkDeviceSize newSize = (m_TransparentInstanceCapacity == 0) ? transparentRequired * 2 : m_TransparentInstanceCapacity * 2;
        if (newSize < transparentRequired) newSize = transparentRequired;
        VkBuffer newBuffer = VK_NULL_HANDLE;
        VkDeviceMemory newMemory = VK_NULL_HANDLE;
        VkBufferCreateInfo bufInfo = {};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = newSize;
        bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &newBuffer) != VK_SUCCESS) return;
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(g_Device, newBuffer, &memReq);
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &newMemory) != VK_SUCCESS) {
            vkDestroyBuffer(g_Device, newBuffer, g_Allocator);
            return;
        }
        vkBindBufferMemory(g_Device, newBuffer, newMemory, 0);
        void* mapped = nullptr;
        vkMapMemory(g_Device, newMemory, 0, newSize, 0, &mapped);

        if (m_TransparentInstanceBuffer != VK_NULL_HANDLE) {
            m_RetiredBuffers.push_back({ m_TransparentInstanceBuffer, m_TransparentInstanceBufferMemory, m_TransparentInstanceMapped, 2 });
        }
        m_TransparentInstanceBuffer = newBuffer;
        m_TransparentInstanceBufferMemory = newMemory;
        m_TransparentInstanceMapped = mapped;
        m_TransparentInstanceCapacity = newSize;
    }
}
void WorldRenderer::Cleanup()
{
    if (m_QuadVertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_QuadVertexBuffer, g_Allocator);
        m_QuadVertexBuffer = VK_NULL_HANDLE;
    }
    if (m_QuadVertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_QuadVertexBufferMemory, g_Allocator);
        m_QuadVertexBufferMemory = VK_NULL_HANDLE;
    }

    // 销毁三个实例缓冲
    if (m_InstanceMapped) { vkUnmapMemory(g_Device, m_InstanceBufferMemory); m_InstanceMapped = nullptr; }
    if (m_InstanceBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(g_Device, m_InstanceBuffer, g_Allocator); m_InstanceBuffer = VK_NULL_HANDLE; }
    if (m_InstanceBufferMemory != VK_NULL_HANDLE) { vkFreeMemory(g_Device, m_InstanceBufferMemory, g_Allocator); m_InstanceBufferMemory = VK_NULL_HANDLE; }
    m_InstanceCapacity = 0;
    if (m_AlphaInstanceMapped) { vkUnmapMemory(g_Device, m_AlphaInstanceBufferMemory); m_AlphaInstanceMapped = nullptr; }
    if (m_AlphaInstanceBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(g_Device, m_AlphaInstanceBuffer, g_Allocator); m_AlphaInstanceBuffer = VK_NULL_HANDLE; }
    if (m_AlphaInstanceBufferMemory != VK_NULL_HANDLE) { vkFreeMemory(g_Device, m_AlphaInstanceBufferMemory, g_Allocator); m_AlphaInstanceBufferMemory = VK_NULL_HANDLE; }
    m_AlphaInstanceCapacity = 0;
    if (m_TransparentInstanceMapped) { vkUnmapMemory(g_Device, m_TransparentInstanceBufferMemory); m_TransparentInstanceMapped = nullptr; }
    if (m_TransparentInstanceBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(g_Device, m_TransparentInstanceBuffer, g_Allocator); m_TransparentInstanceBuffer = VK_NULL_HANDLE; }
    if (m_TransparentInstanceBufferMemory != VK_NULL_HANDLE) { vkFreeMemory(g_Device, m_TransparentInstanceBufferMemory, g_Allocator); m_TransparentInstanceBufferMemory = VK_NULL_HANDLE; }
    m_TransparentInstanceCapacity = 0;

    // 销毁退役队列中的所有缓冲（Cleanup 时 GPU 已空闲）
    for (auto& r : m_RetiredBuffers) {
        if (r.mapped) vkUnmapMemory(g_Device, r.memory);
        if (r.buffer != VK_NULL_HANDLE) vkDestroyBuffer(g_Device, r.buffer, g_Allocator);
        if (r.memory != VK_NULL_HANDLE) vkFreeMemory(g_Device, r.memory, g_Allocator);
    }
    m_RetiredBuffers.clear();

    m_Pipeline.Cleanup();
    m_AlphaPipeline.Cleanup();
    m_TransparentPipeline.Cleanup();
    m_Descriptor.Cleanup();
    m_DescriptorSet = VK_NULL_HANDLE;

    if (m_AtlasSampler != VK_NULL_HANDLE) { vkDestroySampler(g_Device, m_AtlasSampler, g_Allocator); m_AtlasSampler = VK_NULL_HANDLE; }
    if (m_AtlasImageView != VK_NULL_HANDLE) { vkDestroyImageView(g_Device, m_AtlasImageView, g_Allocator); m_AtlasImageView = VK_NULL_HANDLE; }
    if (m_AtlasImage != VK_NULL_HANDLE) { vkDestroyImage(g_Device, m_AtlasImage, g_Allocator); m_AtlasImage = VK_NULL_HANDLE; }
    if (m_AtlasImageMemory != VK_NULL_HANDLE) { vkFreeMemory(g_Device, m_AtlasImageMemory, g_Allocator); m_AtlasImageMemory = VK_NULL_HANDLE; }

    m_Initialized = false;
}

// ---------------- 内部实现 ----------------

bool WorldRenderer::CreateQuadBuffer()
{
    // 单元 quad：TRIANGLE_STRIP 4 顶点（与旧版一致）
    const float quadVertices[] = {
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
        -0.5f,  0.5f, 0.0f,
         0.5f,  0.5f, 0.0f,
    };
    VkDeviceSize bufferSize = sizeof(quadVertices);

    // staging
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &stagingBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        return false;
    }
    vkBindBufferMemory(g_Device, stagingBuffer, stagingMemory, 0);
    void* data = nullptr;
    vkMapMemory(g_Device, stagingMemory, 0, bufferSize, 0, &data);
    memcpy(data, quadVertices, bufferSize);
    vkUnmapMemory(g_Device, stagingMemory);

    // DEVICE_LOCAL 目标
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_QuadVertexBuffer) != VK_SUCCESS) {
        vkFreeMemory(g_Device, stagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        return false;
    }
    vkGetBufferMemoryRequirements(g_Device, m_QuadVertexBuffer, &memReq);
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_QuadVertexBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, m_QuadVertexBuffer, g_Allocator);
        vkFreeMemory(g_Device, stagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        return false;
    }
    vkBindBufferMemory(g_Device, m_QuadVertexBuffer, m_QuadVertexBufferMemory, 0);

    CopyBuffer(m_QuadVertexBuffer, stagingBuffer, bufferSize);

    vkFreeMemory(g_Device, stagingMemory, g_Allocator);
    vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
    return true;
}

bool WorldRenderer::LoadAtlasTexture()
{
    std::string texPath = EngineConfig::GetEngineTexturePath("Blocks.png");
    SDL_Surface* surface = IMG_Load(texPath.c_str());
    if (surface == nullptr) {
        printf("[WorldRenderer] Failed to load atlas: %s (SDL: %s)",
            texPath.c_str(), SDL_GetError());
        return false;
    }

    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
    SDL_DestroySurface(surface);
    if (converted == nullptr) {
        printf("[WorldRenderer] Failed to convert atlas surface");
        return false;
    }

    int width = converted->w;
    int height = converted->h;
    int pitch = converted->pitch;
    unsigned char* pixels = static_cast<unsigned char*>(converted->pixels);
    m_AtlasWidth = (uint32_t)width;
    m_AtlasHeight = (uint32_t)height;

    // 通道重排 (SDL RGBA8888 小端内存为 ABGR)
    // 注意：不翻转 Y —— 保持 SDL 行序（图像顶部在内存顶部），
    // 配合 world.vert 中 baseUV.y 翻转，与旧版 OpenGL 语义一致（t=0=图像顶部）
    std::vector<unsigned char> imageData(width * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char a = pixels[y * pitch + x * 4 + 0];
            unsigned char b = pixels[y * pitch + x * 4 + 1];
            unsigned char g = pixels[y * pitch + x * 4 + 2];
            unsigned char r = pixels[y * pitch + x * 4 + 3];
            imageData[(y * width + x) * 4 + 0] = r;
            imageData[(y * width + x) * 4 + 1] = g;
            imageData[(y * width + x) * 4 + 2] = b;
            imageData[(y * width + x) * 4 + 3] = a;
        }
    }
    SDL_DestroySurface(converted);

    // 创建 Vulkan 图像
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(width);
    imageInfo.extent.height = static_cast<uint32_t>(height);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_AtlasImage) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(g_Device, m_AtlasImage, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_AtlasImageMemory) != VK_SUCCESS) {
        vkDestroyImage(g_Device, m_AtlasImage, g_Allocator);
        m_AtlasImage = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(g_Device, m_AtlasImage, m_AtlasImageMemory, 0);

    // image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_AtlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_AtlasImageView) != VK_SUCCESS) return false;

    // sampler（像素风：NEAREST + CLAMP）
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    if (vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &m_AtlasSampler) != VK_SUCCESS) return false;

    // 上传：UNDEFINED -> TRANSFER_DST -> 拷贝 -> SHADER_READ_ONLY
    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandPool = g_CommandPool;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g_Device, &cmdAlloc, &cmd);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier toDst = {};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = m_AtlasImage;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.baseMipLevel = 0;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.baseArrayLayer = 0;
    toDst.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toDst);

    // staging buffer
    VkDeviceSize imageSize = imageData.size();
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = imageSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &stagingBuffer) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &memReq);
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingMemory) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        return false;
    }
    vkBindBufferMemory(g_Device, stagingBuffer, stagingMemory, 0);
    void* mapped = nullptr;
    vkMapMemory(g_Device, stagingMemory, 0, imageSize, 0, &mapped);
    memcpy(mapped, imageData.data(), imageSize);
    vkUnmapMemory(g_Device, stagingMemory);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_AtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = {};
    toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = m_AtlasImage;
    toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toShader.subresourceRange.baseMipLevel = 0;
    toShader.subresourceRange.levelCount = 1;
    toShader.subresourceRange.baseArrayLayer = 0;
    toShader.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toShader);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);

    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);
    vkFreeMemory(g_Device, stagingMemory, g_Allocator);
    vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
    return true;
}

bool WorldRenderer::CreateDescriptorSet()
{
    m_Descriptor.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!m_Descriptor.CreateLayout()) return false;
    if (!m_Descriptor.CreatePool(1)) return false;
    if (!m_Descriptor.AllocateSet(m_DescriptorSet)) return false;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_AtlasImageView;
    imageInfo.sampler = m_AtlasSampler;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_DescriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);
    return true;
}

bool WorldRenderer::CreatePipeline(VkRenderPass renderPass)
{
    PipelineConfig config;
    config.vertShader = "world.vert.spv";
    config.fragShader = "world.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    // 与旧版一致：opaque 单面剔除（OpenGL CULL_FACE 开启）
    config.cullMode = VK_CULL_MODE_BACK_BIT;
    config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    config.depthTest = true;
    config.depthWrite = true;
    config.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    config.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        0
    };
    config.subpass = 1;              // MRT 几何 subpass（0=z-prepass）
    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // z-prepass 后必须 <=

    VkVertexInputBindingDescription bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].stride = 3 * sizeof(float);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(Chunk::FaceInstance); // 16 字节: pos(12) + packedData(4)
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[3] = {};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].binding = 1;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Chunk::FaceInstance, posX);
    attributes[2].binding = 1;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32_UINT;
    attributes[2].offset = offsetof(Chunk::FaceInstance, packedData);

    config.vertexBindings.assign(bindings, bindings + 2);
    config.vertexAttributes.assign(attributes, attributes + 3);

    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(WorldPushConstants);
    config.usePushConstants = true;

    if (!m_Pipeline.Create(renderPass, m_Descriptor.GetLayout(), config)) {
        printf("[WorldRenderer] Failed to create opaque pipeline");
        return false;
    }

    // 植物/树叶管线（alpha 剔除，双面：与旧版 alphatest pass 关 cull 一致）
    PipelineConfig alphaConfig = config;
    alphaConfig.cullMode = VK_CULL_MODE_NONE;
    if (!m_AlphaPipeline.Create(renderPass, m_Descriptor.GetLayout(), alphaConfig)) {
        printf("[WorldRenderer] Failed to create alpha pipeline");
        return false;
    }

    // 透明管线（水）：alpha 混合，不写深度（旧版 transparent pass 开 cull + blend）
    PipelineConfig transparentConfig = config;
    transparentConfig.blending = true;
    transparentConfig.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    transparentConfig.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    transparentConfig.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    transparentConfig.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    transparentConfig.depthWrite = false;
    if (!m_TransparentPipeline.Create(renderPass, m_Descriptor.GetLayout(), transparentConfig)) {
        printf("[WorldRenderer] Failed to create transparent pipeline");
        return false;
    }
    return true;
}

void WorldRenderer::RecycleRetiredBuffers()
{
    for (auto it = m_RetiredBuffers.begin(); it != m_RetiredBuffers.end();) {
        if (--it->framesLeft <= 0) {
            if (it->mapped) vkUnmapMemory(g_Device, it->memory);
            if (it->buffer != VK_NULL_HANDLE) vkDestroyBuffer(g_Device, it->buffer, g_Allocator);
            if (it->memory != VK_NULL_HANDLE) vkFreeMemory(g_Device, it->memory, g_Allocator);
            it = m_RetiredBuffers.erase(it);
        } else {
            ++it;
        }
    }
}
