#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "Core/RenderGlobals.h"
#include "VulkanManager.h"

#include <cstdio>
#include <unordered_map>

void ModelRenderer::RenderInstanced(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& view, const glm::mat4& proj,
                                     const std::vector<ModelInstanceData>& instanceData,
                                     const ECS::MaterialComponent* material)
{
    if (m_ModelData.pipeline.GetPipeline() == VK_NULL_HANDLE ||
        m_ModelData.pipeline.GetLayout() == VK_NULL_HANDLE ||
        instanceData.empty() ||
        m_ModelData.subMeshes.empty()) {
        return;
    }

    ModelUniformData ubo = {};
    ubo.projView = proj * view;
    ubo.taaJitter = g_CurrentTAAJitter;
    ubo.prevProjView = ubo.projView; // 简单版本没有上一帧数据

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

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ModelData.pipeline.GetPipeline());

    vkCmdPushConstants(commandBuffer, m_ModelData.pipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ModelUniformData), &ubo);

    // 如果排序索引需要重建（脏标记），则重建
    if (m_ModelData.sortedIndicesDirty || m_ModelData.cachedSortedIndices.empty()) {
        BuildSortedIndices();
    }

    // 使用缓存的排序索引，避免每帧重复创建和排序
    VkDescriptorSet currentDescriptorSet = VK_NULL_HANDLE;

    for (size_t sortedIdx : m_ModelData.cachedSortedIndices) {
        const auto& subMesh = m_ModelData.subMeshes[sortedIdx];
        (void)subMesh;

        // 只在描述符集变化时才绑定
        const VkDescriptorSet curSet = subMesh.descriptorSet;
        if (curSet != currentDescriptorSet) {
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_ModelData.pipeline.GetLayout(),
                0, 1, &curSet, 1, &g_BoneDynamicOffset);
            currentDescriptorSet = curSet;
        }

        VkBuffer vertexBuffers[2] = {subMesh.vertexBuffer, m_ModelData.instanceBuffer};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ModelData.pipeline.GetPipeline());
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);

        vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        PushSubMeshMaterialParams(commandBuffer, m_ModelData.pipeline.GetLayout(), subMesh);

        vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
    }
}

void ModelRenderer::RenderInstanced(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& projView, const glm::mat4& prevProjView,
                                     const glm::vec3& cameraPosition,
                                     const std::vector<ModelInstanceData>& instanceData,
                                     const ECS::MaterialComponent* material,
                                     const std::vector<size_t>& visibleSubMeshIndices)
{
    (void)material;
    if (m_ModelData.pipeline.GetPipeline() == VK_NULL_HANDLE ||
        m_ModelData.pipeline.GetLayout() == VK_NULL_HANDLE ||
        instanceData.empty()) {
        return;
    }

    // [diag-20260806] GPU 蒙皮实证：CPU 复算顶点 0 蒙皮结果（验证 boneMatrices 数据 → skinPos 是否合理）
    static bool s_skinDiag = true;
    if (s_skinDiag && m_ModelData.hasSkinning && !m_MeshData.subMeshes.empty()) {
        s_skinDiag = false;
        const auto& sm0 = m_MeshData.subMeshes[0];
        if (!sm0.vertices.empty()) {
            const Vertex& v0 = sm0.vertices[0];
            const glm::vec4 w0 = glm::vec4(v0.BoneWeights) * (1.0f / 255.0f);   // UNORM 解包
            glm::mat4 s(0.0f);
            if (w0.x > 0) s += w0.x * m_ModelData.boneMatrices[v0.BoneIDs.x & (MAX_BONES - 1)];
            if (w0.y > 0) s += w0.y * m_ModelData.boneMatrices[v0.BoneIDs.y & (MAX_BONES - 1)];
            if (w0.z > 0) s += w0.z * m_ModelData.boneMatrices[v0.BoneIDs.z & (MAX_BONES - 1)];
            if (w0.w > 0) s += w0.w * m_ModelData.boneMatrices[v0.BoneIDs.w & (MAX_BONES - 1)];
            glm::vec4 sp = s * glm::vec4(v0.Position, 1.0f);
            const glm::mat4& bm = m_ModelData.boneMatrices[v0.BoneIDs.x & (MAX_BONES - 1)];
            printf("[diag] v0 pos=(%.2f,%.2f,%.2f) ids=(%u,%u,%u,%u) w=(%.2f,%.2f,%.2f,%.2f)\n",
                v0.Position.x, v0.Position.y, v0.Position.z,
                v0.BoneIDs.x, v0.BoneIDs.y, v0.BoneIDs.z, v0.BoneIDs.w,
                w0.x, w0.y, w0.z, w0.w);
            printf("[diag] skinPos=(%.2f,%.2f,%.2f) bm[ids.x].c0=(%.3f,%.3f,%.3f,%.3f)\n",
                sp.x, sp.y, sp.z, bm[0][0], bm[0][1], bm[0][2], bm[0][3]);
            fflush(stdout);
        }
    }
    ModelUniformData ubo = {};
    ubo.projView = projView;
    ubo.taaJitter = g_CurrentTAAJitter;
    ubo.prevProjView = prevProjView;
    ubo.cameraPosition = cameraPosition;

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

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ModelData.pipeline.GetPipeline());

    vkCmdPushConstants(commandBuffer, m_ModelData.pipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ModelUniformData), &ubo);

    // 如果 visibleSubMeshIndices 为空，渲染所有 submesh
    const std::vector<size_t>* indicesToRender = &visibleSubMeshIndices;
    std::vector<size_t> allIndices;
    if (visibleSubMeshIndices.empty()) {
        allIndices.reserve(m_ModelData.subMeshes.size());
        for (size_t i = 0; i < m_ModelData.subMeshes.size(); ++i) {
            allIndices.push_back(i);
        }
        indicesToRender = &allIndices;
    }

    // 按描述符集分组子网格，减少描述符切换次数
    std::unordered_map<VkDescriptorSet, std::vector<size_t>> descriptorSetGroups;
    for (size_t subMeshIdx : *indicesToRender) {
        if (subMeshIdx >= m_ModelData.subMeshes.size()) continue;
        const auto& subMesh = m_ModelData.subMeshes[subMeshIdx];
        // 分组 key 按 descriptor set
        const VkDescriptorSet groupSet = subMesh.descriptorSet;
        if (groupSet != VK_NULL_HANDLE) {
            descriptorSetGroups[groupSet].push_back(subMeshIdx);
        }
    }

    // 按描述符集分组渲染
    for (const auto& [descriptorSet, subMeshIndices] : descriptorSetGroups) {
        // 绑定描述符集（每个组只绑定一次；静态组用静态 layout 与管线/shader 严格匹配，RenderDoc 回放兼容）
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_ModelData.pipeline.GetLayout(), 0, 1, &descriptorSet, 1, &g_BoneDynamicOffset);

        // 渲染该描述符集下的所有子网格
        for (size_t subMeshIdx : subMeshIndices) {
            const auto& subMesh = m_ModelData.subMeshes[subMeshIdx];

            // 绑定顶点和索引缓冲区（GPU 蒙皮用原始顶点缓冲+shader 蒙皮；CPU fallback 用预蒙皮顶点缓冲）
            const bool useSkinned = !g_UseGpuSkinning && m_ModelData.hasSkinning &&
                m_ModelData.skinnedVertexBuffer != VK_NULL_HANDLE &&
                subMeshIdx < m_ModelData.skinnedBufferOffsets.size();
            VkBuffer vertexBuffers[2] = {
                useSkinned ? m_ModelData.skinnedVertexBuffer : subMesh.vertexBuffer,
                m_ModelData.instanceBuffer };
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ModelData.pipeline.GetPipeline());
            VkDeviceSize offsets[] = {
                useSkinned ? m_ModelData.skinnedBufferOffsets[subMeshIdx] : 0,
                0 };
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);

            vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            PushSubMeshMaterialParams(commandBuffer, m_ModelData.pipeline.GetLayout(), subMesh);

            // 绘制子网格
            vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
        }
    }
}
