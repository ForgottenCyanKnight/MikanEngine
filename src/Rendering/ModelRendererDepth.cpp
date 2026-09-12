#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "Core/RenderGlobals.h"
#include "VulkanManager.h"

#include <algorithm>
#include <set>

// ===== z-prepass（subpass 0，depth-only）：只写深度，无材质采样 =====
// MRT 阶段（subpass 1）被遮挡片元在 fragment shader 执行前被深度测试剔除，减少 G-Buffer 4 附件写入的 overdraw。
// 与主几何共用顶点缓冲/实例缓冲/索引缓冲（统一蒙皮 32B）。
void ModelRenderer::RenderDepthOnly(VkCommandBuffer commandBuffer, int width, int height,
                                    const glm::mat4& projView,
                                    const std::vector<ModelInstanceData>& instanceData,
                                    const std::vector<size_t>& visibleSubMeshIndices)
{
    if (m_ModelData.depthPipeline.GetPipeline() == VK_NULL_HANDLE ||
        m_ModelData.depthPipeline.GetLayout() == VK_NULL_HANDLE ||
        instanceData.empty() ||
        m_ModelData.subMeshes.empty()) {
        return;
    }

    ModelUniformData ubo = {};
    ubo.projView = projView;
    ubo.taaJitter = g_CurrentTAAJitter;
    ubo.prevProjView = projView;   // z-prepass 不需要运动矢量
    UpdateInstanceBuffer(instanceData);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    if (!m_ModelData.batchGroups.empty()) {
        const bool hasVis = !visibleSubMeshIndices.empty();
        std::set<size_t> vis;
        if (hasVis) vis.insert(visibleSubMeshIndices.begin(), visibleSubMeshIndices.end());
        for (const auto& group : m_ModelData.batchGroups) {
            if (group.vertexBuffer == VK_NULL_HANDLE || group.indexBuffer == VK_NULL_HANDLE || group.items.empty()) continue;
            const VkPipeline depthPipe = m_ModelData.depthPipeline.GetPipeline();
            const VkPipelineLayout depthLayout = m_ModelData.depthPipeline.GetLayout();
            if (depthPipe == VK_NULL_HANDLE || depthLayout == VK_NULL_HANDLE) continue;
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPipe);
            vkCmdPushConstants(commandBuffer, depthLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ModelUniformData), &ubo);

                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    depthLayout, 0, 1, &group.descriptorSet, 1, &g_BoneDynamicOffset);
            

            uint32_t segFirst = 0, segCount = 0;
            bool hasSeg = false;
            const SubMeshRenderData* segHead = nullptr;
            auto flushSeg = [&]() {
                if (!hasSeg) return;
                glm::vec4 zpreAlpha(segHead->alphaCutoff, (float)segHead->alphaMode, 0.0f, 0.0f);
                vkCmdPushConstants(commandBuffer, depthLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData), 16, &zpreAlpha);
                VkBuffer vertexBuffers[2] = {group.vertexBuffer, m_ModelData.instanceBuffer};
                VkDeviceSize offsets[] = {0, 0};
                vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
                vkCmdBindIndexBuffer(commandBuffer, group.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, segCount, static_cast<uint32_t>(instanceData.size()), segFirst, 0, 0);
                hasSeg = false;
            };
            for (const auto& item : group.items) {
                const auto& sm = m_ModelData.subMeshes[item.subMeshIndex];
                // BLEND 不写深度（与逐 subMesh 语义一致）+ 视锥外跳过 → 断段
                if (sm.alphaMode == 2 || (hasVis && vis.find(item.subMeshIndex) == vis.end())) {
                    flushSeg();
                    continue;
                }
                if (!hasSeg) {
                    segFirst = item.firstIndex; segCount = item.indexCount; segHead = &sm; hasSeg = true;
                } else if (item.firstIndex == segFirst + segCount) {
                    segCount += item.indexCount;
                } else {
                    flushSeg();
                    segFirst = item.firstIndex; segCount = item.indexCount; segHead = &sm; hasSeg = true;
                }
            }
            flushSeg();
        }
    }

    for (size_t sortedIdx : m_ModelData.cachedSortedIndices) {
        if (!visibleSubMeshIndices.empty() &&
            std::find(visibleSubMeshIndices.begin(), visibleSubMeshIndices.end(), sortedIdx) == visibleSubMeshIndices.end()) {
            continue;
        }
        const auto& subMesh = m_ModelData.subMeshes[sortedIdx];
        if (m_ModelData.subMeshBatchGroup.size() == m_ModelData.subMeshes.size() &&
            m_ModelData.subMeshBatchGroup[sortedIdx] >= 0) {
            continue;
        }
        if (subMesh.alphaMode == 2) continue;
        const VkPipeline depthPipe =  m_ModelData.depthPipeline.GetPipeline();
        const VkPipelineLayout depthLayout = m_ModelData.depthPipeline.GetLayout();
        if (depthPipe == VK_NULL_HANDLE || depthLayout == VK_NULL_HANDLE) continue;

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPipe);
        vkCmdPushConstants(commandBuffer, depthLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ModelUniformData), &ubo);
        // y=alphaMode：-1 未知→0.5 旧行为；0/2（OPAQUE/BLEND）不采样不 discard；1（MASK）按 cutoff discard
        glm::vec4 zpreAlpha(subMesh.alphaCutoff, (float)subMesh.alphaMode, 0.0f, 0.0f);
        vkCmdPushConstants(commandBuffer, depthLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData), 16, &zpreAlpha);

            const VkDescriptorSet curSet = subMesh.descriptorSet;
            if (curSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    depthLayout, 0, 1, &curSet, 1, &g_BoneDynamicOffset);
            }
        

        VkBuffer vertexBuffers[2] = {VK_NULL_HANDLE, m_ModelData.instanceBuffer};
        vertexBuffers[0] = subMesh.vertexBuffer;
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
    }
}
