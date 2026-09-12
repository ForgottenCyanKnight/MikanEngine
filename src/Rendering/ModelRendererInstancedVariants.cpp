#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "Core/RenderGlobals.h"
#include "VulkanManager.h"

#include <algorithm>
#include <map>
#include <unordered_map>

void ModelRenderer::RenderInstancedBatches(VkCommandBuffer commandBuffer, int width, int height,
                                            const glm::mat4& projView, const glm::mat4& prevProjView,
                                            const glm::vec3& cameraPosition,
                                            const std::vector<SubMeshInstanceData>& batches,
                                            const ECS::MaterialComponent* material,
                                            bool doubleSided, bool wireframe)
{
    (void)material;
    const VulkanPipeline& pipeline = wireframe ? m_ModelData.wireframePipeline
        : doubleSided ? m_ModelData.doubleSidedPipeline
        : m_ModelData.pipeline;
    if (pipeline.GetPipeline() == VK_NULL_HANDLE || pipeline.GetLayout() == VK_NULL_HANDLE || batches.empty()) {
        return;
    }

    // 合并所有批次的实例到单一 instance buffer，记录每个 submesh 的实例偏移(firstInstance)
    struct BatchDraw { size_t subMeshIndex; uint32_t firstInstance; uint32_t instanceCount; int batchGroup; size_t batchIndex; };
    std::vector<BatchDraw> draws;
    size_t totalInstances = 0;
    // 组内实例共享：同组相邻可见 subMesh 若实例内容相同 → firstInstance 相同（一次 draw 覆盖多 subMesh）
    std::map<int, size_t> groupLastBatch;   // group → batches 索引（比较实例内容）
    std::map<int, uint32_t> groupLastInst;  // group → 共享实例偏移
    auto instancesEqual = [](const std::vector<ModelInstanceData>& a, const std::vector<ModelInstanceData>& b) -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].model != b[i].model) return false;
            if (a[i].prevModel != b[i].prevModel) return false;
            if (a[i].albedoColor != b[i].albedoColor) return false;
            if (a[i].materialData != b[i].materialData) return false;
            if (a[i].textureFlags != b[i].textureFlags) return false;
        }
        return true;
    };
    for (size_t bi = 0; bi < batches.size(); ++bi) {
        const auto& batch = batches[bi];
        if (batch.instances.empty()) continue;
        int bg = -1;
        if (m_ModelData.subMeshBatchGroup.size() == m_ModelData.subMeshes.size() &&
            batch.subMeshIndex < m_ModelData.subMeshBatchGroup.size()) {
            bg = m_ModelData.subMeshBatchGroup[batch.subMeshIndex];
        }
        if (bg >= 0) {
            auto prevIt = groupLastBatch.find(bg);
            if (prevIt != groupLastBatch.end() &&
                instancesEqual(batches[prevIt->second].instances, batch.instances)) {
                // 与组内上一可见 subMesh 实例相同 → 共享同一实例块
                draws.push_back({ batch.subMeshIndex, groupLastInst[bg], (uint32_t)batch.instances.size(), bg, bi });
                continue;
            }
        }
        draws.push_back({ batch.subMeshIndex, (uint32_t)totalInstances, (uint32_t)batch.instances.size(), bg, bi });
        if (bg >= 0) {
            groupLastBatch[bg] = bi;
            groupLastInst[bg] = (uint32_t)totalInstances;
        }
        totalInstances += batch.instances.size();
    }
    if (draws.empty()) return;

    std::map<size_t, size_t> batchBySub;
    for (size_t i = 0; i < batches.size(); ++i) batchBySub[batches[i].subMeshIndex] = i;

    std::vector<ModelInstanceData> allInstances;
    allInstances.reserve(totalInstances);
    for (const auto& d : draws) {
        auto it = batchBySub.find(d.subMeshIndex);
        if (it == batchBySub.end()) continue;
        allInstances.insert(allInstances.end(),
            batches[it->second].instances.begin(), batches[it->second].instances.end());
    }
    UpdateInstanceBuffer(allInstances);

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

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipeline());

    ModelUniformData ubo = {};
    ubo.projView = projView;
    ubo.taaJitter = g_CurrentTAAJitter;
    ubo.prevProjView = prevProjView;
    ubo.cameraPosition = cameraPosition;
    vkCmdPushConstants(commandBuffer, pipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ModelUniformData), &ubo);

    // 按描述符集(材质)分组批次，组间才切换描述符，组内连续绘制
    std::unordered_map<VkDescriptorSet, std::vector<size_t>> descriptorSetGroups;
    for (size_t i = 0; i < draws.size(); ++i) {
        if (draws[i].subMeshIndex >= m_ModelData.subMeshes.size()) continue;
        const auto& subMesh = m_ModelData.subMeshes[draws[i].subMeshIndex];
        const VkDescriptorSet groupSet = subMesh.descriptorSet;
        if (groupSet != VK_NULL_HANDLE) {
            descriptorSetGroups[groupSet].push_back(i);
        }
    }

    if (!m_ModelData.batchGroups.empty()) {
        std::map<size_t, size_t> subToDraw;
        for (size_t i = 0; i < draws.size(); ++i) subToDraw[draws[i].subMeshIndex] = i;

        struct SegCmd {
            float dist;
            uint32_t firstIndex, indexCount, firstInstance, instanceCount;
            const SubMeshRenderData* head;
        };

        for (const auto& group : m_ModelData.batchGroups) {
            if (group.vertexBuffer == VK_NULL_HANDLE || group.indexBuffer == VK_NULL_HANDLE || group.items.empty()) continue;
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipeline());
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline.GetLayout(), 0, 1,
                &group.descriptorSet, 1, &g_BoneDynamicOffset);

            std::vector<SegCmd> segs;
            uint32_t segFirst = 0, segCount = 0, segFirstInst = 0, segInstCount = 0;
            bool hasSeg = false;
            float segDist = 0.0f;
            const SubMeshRenderData* segHead = nullptr;
            auto flushSeg = [&]() {
                if (!hasSeg) return;
                segs.push_back({ segDist, segFirst, segCount, segFirstInst, segInstCount, segHead });
                hasSeg = false;
            };

            for (const auto& item : group.items) {
                auto it = subToDraw.find(item.subMeshIndex);
                if (it == subToDraw.end()) {   // 不可见 → 断段
                    flushSeg();
                    continue;
                }
                const auto& d = draws[it->second];
                const auto& sm = m_ModelData.subMeshes[item.subMeshIndex];
                // 段距离 = 段内最近 subMesh 中心到相机（近→远排序，early-z 尽早写深度）
                float itemDist = 0.0f;
                if (item.subMeshIndex < m_BVHData.subMeshAABBs.size() &&
                    d.batchIndex < batches.size() && !batches[d.batchIndex].instances.empty()) {
                    const glm::vec3 localC = m_BVHData.subMeshAABBs[item.subMeshIndex].GetCenter();
                    const glm::vec3 worldC = glm::vec3(
                        batches[d.batchIndex].instances[0].model * glm::vec4(localC, 1.0f));
                    itemDist = glm::length(worldC - cameraPosition);
                }
                if (!hasSeg) {
                    segFirst = item.firstIndex; segCount = item.indexCount;
                    segFirstInst = d.firstInstance; segInstCount = d.instanceCount;
                    segHead = &sm; segDist = itemDist; hasSeg = true;
                } else if (d.instanceCount == segInstCount &&
                           d.firstInstance == segFirstInst &&
                           item.firstIndex == segFirst + segCount) {
                    segCount += item.indexCount;
                    if (itemDist < segDist) segDist = itemDist;
                } else {
                    flushSeg();
                    segFirst = item.firstIndex; segCount = item.indexCount;
                    segFirstInst = d.firstInstance; segInstCount = d.instanceCount;
                    segHead = &sm; segDist = itemDist; hasSeg = true;
                }
            }
            flushSeg();
            if (segs.empty()) continue;
            // 段级近→远排序（stable——等距保持原相对序，合批段间材质一致不受影响）
            std::stable_sort(segs.begin(), segs.end(),
                [](const SegCmd& a, const SegCmd& b) { return a.dist < b.dist; });

            VkBuffer vertexBuffers[2] = {group.vertexBuffer, m_ModelData.instanceBuffer};
            VkDeviceSize offsets[] = {0, 0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer, group.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            for (const auto& s : segs) {
                PushSubMeshMaterialParams(commandBuffer, pipeline.GetLayout(), *s.head, doubleSided);
                vkCmdDrawIndexed(commandBuffer, s.indexCount, s.instanceCount, s.firstIndex, 0, s.firstInstance);
            }
        }
    }

    // 原路径：仅未合批的 subMesh（subMeshBatchGroup == -1；合批组内的跳过——已在上方绘制）
    for (const auto& [descriptorSet, drawIndices] : descriptorSetGroups) {
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline.GetLayout(), 0, 1, &descriptorSet, 1, &g_BoneDynamicOffset);

        for (size_t i : drawIndices) {
            const auto& subMesh = m_ModelData.subMeshes[draws[i].subMeshIndex];
            if (m_ModelData.subMeshBatchGroup.size() == m_ModelData.subMeshes.size() &&
                m_ModelData.subMeshBatchGroup[draws[i].subMeshIndex] >= 0) {
                continue;
            }

            VkBuffer vertexBuffers[2] = {subMesh.vertexBuffer, m_ModelData.instanceBuffer};
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipeline());
            VkDeviceSize offsets[] = {0, 0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);

            if (subMesh.indexBuffer == VK_NULL_HANDLE || subMesh.indexCount == 0 ||
                subMesh.vertexBuffer == VK_NULL_HANDLE) {
                continue;
            }

            vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            PushSubMeshMaterialParams(commandBuffer, pipeline.GetLayout(), subMesh, doubleSided);

            vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, draws[i].instanceCount, 0, 0, draws[i].firstInstance);
        }
    }
}

void ModelRenderer::RenderInstancedDoubleSided(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& projView, const glm::mat4& prevProjView,
                                     const glm::vec3& cameraPosition,
                                     const std::vector<ModelInstanceData>& instanceData,
                                     const ECS::MaterialComponent* material,
                                     const std::vector<size_t>& visibleSubMeshIndices)
{
    (void)material;
    if (m_ModelData.doubleSidedPipeline.GetPipeline() == VK_NULL_HANDLE ||
        m_ModelData.doubleSidedPipeline.GetLayout() == VK_NULL_HANDLE ||
        instanceData.empty()) {
        return;
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

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ModelData.doubleSidedPipeline.GetPipeline());

    vkCmdPushConstants(commandBuffer, m_ModelData.doubleSidedPipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ModelUniformData), &ubo);

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
        // 绑定描述符集（每个组只绑定一次；静态组用静态 layout）
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_ModelData.doubleSidedPipeline.GetLayout(), 0, 1, &descriptorSet, 1, &g_BoneDynamicOffset);

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
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ModelData.doubleSidedPipeline.GetPipeline());
            VkDeviceSize offsets[] = {
                useSkinned ? m_ModelData.skinnedBufferOffsets[subMeshIdx] : 0,
                0 };
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);

            vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            PushSubMeshMaterialParams(commandBuffer, m_ModelData.doubleSidedPipeline.GetLayout(), subMesh, true);

            // 绘制子网格
            vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
        }
    }
}

void ModelRenderer::RenderInstancedWireframe(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& projView, const glm::mat4& prevProjView,
                                     const glm::vec3& cameraPosition,
                                     const std::vector<ModelInstanceData>& instanceData,
                                     const ECS::MaterialComponent* material,
                                     const std::vector<size_t>& visibleSubMeshIndices)
{
    (void)material;
    if (m_ModelData.wireframePipeline.GetPipeline() == VK_NULL_HANDLE ||
        m_ModelData.wireframePipeline.GetLayout() == VK_NULL_HANDLE ||
        instanceData.empty()) {
        return;
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

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ModelData.wireframePipeline.GetPipeline());

    vkCmdPushConstants(commandBuffer, m_ModelData.wireframePipeline.GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ModelUniformData), &ubo);

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
        const VkDescriptorSet groupSet = subMesh.descriptorSet;
        if (groupSet != VK_NULL_HANDLE) {
            descriptorSetGroups[groupSet].push_back(subMeshIdx);
        }
    }

    // 按描述符集分组渲染
    for (const auto& [descriptorSet, subMeshIndices] : descriptorSetGroups) {
        // 绑定描述符集（每个组只绑定一次；静态组用静态 layout）
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_ModelData.wireframePipeline.GetLayout(), 0, 1, &descriptorSet, 1, &g_BoneDynamicOffset);

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
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ModelData.wireframePipeline.GetPipeline());
            VkDeviceSize offsets[] = {
                useSkinned ? m_ModelData.skinnedBufferOffsets[subMeshIdx] : 0,
                0 };
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);

            vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    PushSubMeshMaterialParams(commandBuffer, m_ModelData.wireframePipeline.GetLayout(), subMesh);

            // 绘制子网格
            vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
        }
    }
}
