#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "Core/Log.h"
#include "VulkanManager.h"

#include <algorithm>

void ModelRenderer::EnsureShadowPipelines(VkRenderPass shadowRenderPass)
{
    if (m_ModelData.shadowDepthPipeline.GetPipeline() != VK_NULL_HANDLE ) {
        return;   // 已创建
    }
    PipelineConfig config;
    config.vertShader = "shadow_depth.vert.spv";
    config.fragShader = "shadow_depth.frag.spv";
    config.cullMode = VK_CULL_MODE_NONE;
    config.colorAttachmentCount = 0;   // depth-only
    config.subpass = 0;
    config.depthCompareOp = VK_COMPARE_OP_LESS;
    config.depthWrite = true;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(glm::mat4) + sizeof(glm::vec4) * 2;   // projView + lightPosRange + subMeshAlpha = 96B
    config.vertexBindings = m_VertexBindings;
    config.vertexAttributes = m_VertexAttributes;
    if (!m_ModelData.shadowDepthPipeline.Create(shadowRenderPass, DescriptorSetCache::GetInstance().GetLayout(), config)) {
        return;
    }
}

void ModelRenderer::RenderShadowDepth(VkCommandBuffer commandBuffer, int width, int height,
                                      const glm::mat4& projView, const glm::vec3& lightPos, float range,
                                      const std::vector<ModelInstanceData>& instanceData,
                                      const std::vector<size_t>& visibleSubMeshIndices)
{
    if (m_ModelData.shadowDepthPipeline.GetPipeline() == VK_NULL_HANDLE ||
        instanceData.empty() || m_ModelData.subMeshes.empty()) {
        return;
    }
    struct ShadowPush {
        glm::mat4 projView;
        glm::vec4 lightPosRange;
        glm::vec4 subMeshAlpha;
    };   // 96B，与 shader push 一致
    ShadowPush push = {};
    push.projView = projView;
    push.lightPosRange = glm::vec4(lightPos, range);
    UpdateInstanceBuffer(instanceData);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    for (size_t sortedIdx : m_ModelData.cachedSortedIndices) {
        if (!visibleSubMeshIndices.empty() &&
            std::find(visibleSubMeshIndices.begin(), visibleSubMeshIndices.end(), sortedIdx) == visibleSubMeshIndices.end()) {
            continue;
        }
        const auto& subMesh = m_ModelData.subMeshes[sortedIdx];
        if (subMesh.alphaMode == 2) {
            // BLEND 没有二值遮罩，不应作为实体写入点光源阴影图。
            continue;
        }
        const VkPipeline pipe = m_ModelData.shadowDepthPipeline.GetPipeline();
        const VkPipelineLayout layout = m_ModelData.shadowDepthPipeline.GetLayout();
        if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) continue;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        push.subMeshAlpha = glm::vec4(subMesh.alphaCutoff, (float)subMesh.alphaMode, 0.0f, 0.0f);
        vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowPush), &push);

            const VkDescriptorSet curSet = subMesh.descriptorSet;   // 蒙皮阴影：bone UBO binding 4
            if (curSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &curSet, 1, &g_BoneDynamicOffset);
            }
        
        VkBuffer vertexBuffers[2] = {VK_NULL_HANDLE, m_ModelData.instanceBuffer};
        vertexBuffers[0] = subMesh.vertexBuffer;
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
    }
}

void ModelRenderer::EnsureCsmPipelines(VkRenderPass csmRenderPass)
{
    if (m_ModelData.csmDepthPipeline.GetPipeline() != VK_NULL_HANDLE ) {
        return;   // 已创建
    }
    PipelineConfig config;
    config.vertShader = "shadow_depth.vert.spv";   // 蒙皮版顶点（输出 vWorldPos 未被 csm frag 使用，无害）
    config.fragShader = "csm_depth.frag.spv";
    config.colorAttachmentCount = 0;   // depth-only
    config.subpass = 0;
    config.depthCompareOp = VK_COMPARE_OP_LESS;
    config.depthWrite = true;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(glm::mat4) + sizeof(glm::vec4) * 2;   // projView + lightPosRange + subMeshAlpha = 96B
    // framebuffer 空间绕序反转 → cull FRONT 实际剔除 GL 背面、渲染 GL 正面（lit≈curD→自阴影）。
    // 改 cull NONE（渲染双面——与点光源阴影 2131 同款已验证）：单面也画、双面深度测试自动取最近面（正面）；
    // 自阴影由 depthBias（2.0/2.0）解决
    config.cullMode = VK_CULL_MODE_NONE;
    config.depthBiasEnable = true;
    config.depthBiasConstantFactor = 2.0f;
    config.depthBiasClamp = 0.0f;
    config.depthBiasSlopeFactor = 2.0f;
    config.vertexBindings = m_VertexBindings;
    config.vertexAttributes = m_VertexAttributes;
    if (!m_ModelData.csmDepthPipeline.Create(csmRenderPass, DescriptorSetCache::GetInstance().GetLayout(), config)) {
        return;
    }
}

void ModelRenderer::RenderCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                                   const glm::mat4& projView,
                                   const std::vector<ModelInstanceData>& instanceData,
                                   const std::vector<size_t>& visibleSubMeshIndices)
{
    if (m_ModelData.csmDepthPipeline.GetPipeline() == VK_NULL_HANDLE ||
        instanceData.empty() || m_ModelData.subMeshes.empty()) {
        return;
    }
    struct CsmPush {
        glm::mat4 projView;
        glm::vec4 unused;
        glm::vec4 subMeshAlpha;
    };   // 96B，与 shader push 一致
    CsmPush push = {};
    push.projView = projView;
    static bool s_pushLogged = false;
    if (!s_pushLogged) {
        s_pushLogged = true;
        LOGI("[CSM-MAT] push c?: m00=%.4f m11=%.4f m22=%.4f m33=%.4f row3=(%.4f,%.4f,%.4f,%.4f)",
             projView[0][0], projView[1][1], projView[2][2], projView[3][3],
             projView[3][0], projView[3][1], projView[3][2], projView[3][3]);
    }
    UpdateInstanceBuffer(instanceData);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    std::vector<uint8_t> visBitmap;
    if (!visibleSubMeshIndices.empty()) {
        visBitmap.assign(m_ModelData.subMeshes.size(), 0);
        for (size_t idx : visibleSubMeshIndices) {
            if (idx < visBitmap.size()) visBitmap[idx] = 1;
        }
    }

    for (size_t sortedIdx : m_ModelData.cachedSortedIndices) {
        if (!visBitmap.empty() && !visBitmap[sortedIdx]) continue;
        const auto& subMesh = m_ModelData.subMeshes[sortedIdx];
        if (subMesh.alphaMode == 2) {
            // BLEND 没有二值遮罩，不应作为实体写入 CSM 阴影图。
            continue;
        }
        const VkPipeline pipe = m_ModelData.csmDepthPipeline.GetPipeline();
        const VkPipelineLayout layout = m_ModelData.csmDepthPipeline.GetLayout();
        if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) continue;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        push.subMeshAlpha = glm::vec4(subMesh.alphaCutoff, (float)subMesh.alphaMode, 0.0f, 0.0f);
        vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CsmPush), &push);
            const VkDescriptorSet curSet = subMesh.descriptorSet;   // 蒙皮阴影：bone UBO binding 4
            if (curSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &curSet, 1, &g_BoneDynamicOffset);
            }
        
        VkBuffer vertexBuffers[2] = {VK_NULL_HANDLE, m_ModelData.instanceBuffer};
        vertexBuffers[0] = subMesh.vertexBuffer;
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
    }
}
