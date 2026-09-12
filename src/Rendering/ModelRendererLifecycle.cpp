#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanContext.h"
#include "VulkanManager.h"

ModelRenderer::ModelRenderer()
    : m_TexturePool(nullptr)
{
}

ModelRenderer::~ModelRenderer()
{
}

void ModelRenderer::Cleanup()
{
    DestroyBatchGroups();
    for (auto& subMesh : m_ModelData.subMeshes) {
        if (subMesh.vertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, subMesh.vertexBuffer, g_Allocator);
        }
        if (subMesh.vertexBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, subMesh.vertexBufferMemory, g_Allocator);
        }
        if (subMesh.indexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, subMesh.indexBuffer, g_Allocator);
        }
        if (subMesh.indexBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, subMesh.indexBufferMemory, g_Allocator);
        }
        // 描述符现在由全局缓存管理，不需要单独清理
        subMesh.descriptorSet = VK_NULL_HANDLE;
    }
    m_ModelData.subMeshes.clear();
    
    if (m_ModelData.uniformBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_ModelData.uniformBuffer, g_Allocator);
    }
    if (m_ModelData.uniformBufferMemory != VK_NULL_HANDLE) {
        if (m_ModelData.boneBufferMapped != nullptr) {
            vkUnmapMemory(g_Device, m_ModelData.uniformBufferMemory);
            m_ModelData.boneBufferMapped = nullptr;
        }
        vkFreeMemory(g_Device, m_ModelData.uniformBufferMemory, g_Allocator);
    }

    // CPU 蒙皮顶点缓冲
    if (m_ModelData.skinnedVertexBufferMemory != VK_NULL_HANDLE) {
        if (m_ModelData.skinnedBufferMapped != nullptr) {
            vkUnmapMemory(g_Device, m_ModelData.skinnedVertexBufferMemory);
            m_ModelData.skinnedBufferMapped = nullptr;
        }
        vkFreeMemory(g_Device, m_ModelData.skinnedVertexBufferMemory, g_Allocator);
    }
    if (m_ModelData.skinnedVertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_ModelData.skinnedVertexBuffer, g_Allocator);
        m_ModelData.skinnedVertexBuffer = VK_NULL_HANDLE;
    }
    if (m_ModelData.boneBufferView != VK_NULL_HANDLE) {
        vkDestroyBufferView(g_Device, m_ModelData.boneBufferView, g_Allocator);
        m_ModelData.boneBufferView = VK_NULL_HANDLE;
    }
    m_ModelData.skinnedSubMeshes.clear();
    m_ModelData.skinnedBufferOffsets.clear();
    
    ModelRendererDetail::ReleaseInstanceUploads(this);
    // 兼容旧的固定槽字段：当前实际上传由外置池持有，这些字段保持为空；
    // 若旧生命周期留下资源，也在这里安全释放，避免重复句柄。
    for (size_t frame = 0; frame < ModelRenderData::MAX_FRAMES_IN_FLIGHT; ++frame) {
        if (m_ModelData.instanceBufferMapped[frame] != nullptr &&
            m_ModelData.instanceBufferMemories[frame] != VK_NULL_HANDLE) {
            vkUnmapMemory(g_Device, m_ModelData.instanceBufferMemories[frame]);
            m_ModelData.instanceBufferMapped[frame] = nullptr;
        }
        if (m_ModelData.instanceBuffers[frame] != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, m_ModelData.instanceBuffers[frame], g_Allocator);
            m_ModelData.instanceBuffers[frame] = VK_NULL_HANDLE;
        }
        if (m_ModelData.instanceBufferMemories[frame] != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, m_ModelData.instanceBufferMemories[frame], g_Allocator);
            m_ModelData.instanceBufferMemories[frame] = VK_NULL_HANDLE;
        }
    }
    m_ModelData.instanceBuffer = VK_NULL_HANDLE;
    m_ModelData.instanceBufferMemory = VK_NULL_HANDLE;
    
    m_ModelData.pipeline.Cleanup();
    m_ModelData.doubleSidedPipeline.Cleanup();  // 清理双面渲染管线
    m_ModelData.wireframePipeline.Cleanup();  // 清理线框渲染管线
    m_ModelData.depthPipeline.Cleanup();
    m_ModelData.shadowDepthPipeline.Cleanup();
    m_ModelData.csmDepthPipeline.Cleanup();
    
    BaseRenderer::Cleanup();
}

void ModelRenderer::Init(VkRenderPass renderPass)
{
    m_TexturePool = g_TexturePool;
    if (m_TexturePool == nullptr) {
        return;
    }
    
    CreatePipeline(renderPass);
    CreateUniformBuffer();
    CreateInstanceBuffer(4096);
}

