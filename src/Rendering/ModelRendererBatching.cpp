#include "ModelRenderer.h"
#include "Core/Log.h"
#include "VulkanManager.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>

namespace {

std::string SubMeshMaterialKey(const SubMeshRenderData& sm) {
    // FBX 同材质在不同 subMesh 的浮点微差会把真材质裂成数百组；渲染差异低于显示精度）
    auto keyf = [](float f) { return std::to_string((int)llroundf(f * 1000.0f)); };
    std::string k;
    k.reserve(256);
    k += "M|";
    k += sm.diffuseTexturePath; k += '|';
    k += sm.normalTexturePath; k += '|';
    k += sm.roughnessTexturePath; k += '|';
    k += sm.metallicTexturePath; k += '|';
    k += sm.emissiveTexturePath; k += '|';
    k += std::to_string(sm.wrapMode); k += '|';
    k += std::to_string(sm.alphaMode); k += '|';
    k += sm.doubleSided ? "D|" : "d|";
    k += keyf(sm.metallic); k += '|';
    k += keyf(sm.roughness); k += '|';
    k += keyf(sm.ao); k += '|';
    k += keyf(sm.mrValid); k += '|';
    k += keyf(sm.alphaCutoff); k += '|';
    k += keyf(sm.diffuseTransmissionFactor);
    return k;
}

} // namespace

void ModelRenderer::DestroyBatchGroups() {
    for (auto& g : m_ModelData.batchGroups) {
        if (g.vertexBuffer) vkDestroyBuffer(g_Device, g.vertexBuffer, g_Allocator);
        if (g.vertexBufferMemory) vkFreeMemory(g_Device, g.vertexBufferMemory, g_Allocator);
        if (g.indexBuffer) vkDestroyBuffer(g_Device, g.indexBuffer, g_Allocator);
        if (g.indexBufferMemory) vkFreeMemory(g_Device, g.indexBufferMemory, g_Allocator);
    }
    m_ModelData.batchGroups.clear();
    m_ModelData.subMeshBatchGroup.clear();
}

void ModelRenderer::RebuildBatchGroups() {
    DestroyBatchGroups();
    const auto& subs = m_ModelData.subMeshes;
    if (subs.empty() || m_MeshData.subMeshes.size() != subs.size()) return;
    // 蒙皮模型每帧 CPU 更新顶点缓冲，与合批静态合并冲突——跳过
    if (m_ModelData.hasSkinning) return;

    // 1) 按材质键分组（保持首见顺序）
    std::map<std::string, size_t> keyToGroup;
    std::vector<size_t> groupOfSub(subs.size(), SIZE_MAX);
    for (size_t i = 0; i < subs.size(); ++i) {
        const auto& sm = subs[i];
        const auto& mesh = m_MeshData.subMeshes[i];
        // 防御：缺 buffer/索引/CPU 顶点的 subMesh 不参与合批
        if (sm.indexBuffer == VK_NULL_HANDLE || sm.indexCount == 0 || mesh.indices.empty()) continue;
        if (sm.vertexBuffer == VK_NULL_HANDLE || mesh.vertices.empty()) continue;
        if (sm.descriptorSet == VK_NULL_HANDLE) continue;

        std::string key = SubMeshMaterialKey(sm);
        auto it = keyToGroup.find(key);
        size_t gidx;
        if (it == keyToGroup.end()) {
            gidx = m_ModelData.batchGroups.size();
            keyToGroup[key] = gidx;
            SubMeshBatchGroup g;
            g.descriptorSet = sm.descriptorSet;
            m_ModelData.batchGroups.push_back(std::move(g));
        } else {
            gidx = it->second;
        }
        groupOfSub[i] = (size_t)gidx;
    }

    // 2) 每组合并顶点/索引（staging 上传，模式与 CreateModelBuffers 一致）
    for (size_t gidx = 0; gidx < m_ModelData.batchGroups.size(); ++gidx) {
        auto& group = m_ModelData.batchGroups[gidx];
        size_t vertBytes = 0, idxCount = 0;
        for (size_t i = 0; i < subs.size(); ++i) {
            if (groupOfSub[i] != gidx) continue;
            const auto& mesh = m_MeshData.subMeshes[i];
            vertBytes += sizeof(Vertex) * mesh.vertices.size();
            idxCount += mesh.indices.size();
        }
        if (vertBytes == 0 || idxCount == 0) continue;

        std::vector<uint8_t> vertData(vertBytes);
        std::vector<uint32_t> idxData(idxCount);
        size_t voff = 0, ioff = 0;
        uint32_t vbase = 0;
        for (size_t i = 0; i < subs.size(); ++i) {
            if (groupOfSub[i] != gidx) continue;
            const auto& mesh = m_MeshData.subMeshes[i];
            const size_t vBytes = sizeof(Vertex) * mesh.vertices.size();
            if (vBytes) {
                memcpy(vertData.data() + voff, (const void*)mesh.vertices.data(), vBytes);
            }
            for (size_t j = 0; j < mesh.indices.size(); ++j) idxData[ioff + j] = mesh.indices[j] + vbase;
            BatchedSubMeshItem item;
            item.subMeshIndex = i;
            item.firstIndex = (uint32_t)ioff;
            item.indexCount = (uint32_t)mesh.indices.size();
            item.vertexOffset = vbase;
            group.items.push_back(item);
            voff += vBytes;
            ioff += mesh.indices.size();
            vbase += (uint32_t)mesh.vertices.size();
        }

        // 创建组 vertex/index buffer（DEVICE_LOCAL + staging 上传）
        auto createDev = [&](VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem, const void* src) -> bool {
            VkBuffer staging; VkDeviceMemory stagingMem;
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = size;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(g_Device, &bi, g_Allocator, &staging) != VK_SUCCESS) return false;
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(g_Device, staging, &mr);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = RendererUtils::FindMemoryType(mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(g_Device, &ai, g_Allocator, &stagingMem) != VK_SUCCESS) {
                vkDestroyBuffer(g_Device, staging, g_Allocator); return false;
            }
            vkBindBufferMemory(g_Device, staging, stagingMem, 0);
            void* data; vkMapMemory(g_Device, stagingMem, 0, size, 0, &data);
            memcpy(data, src, (size_t)size);
            vkUnmapMemory(g_Device, stagingMem);

            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                     | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            if (vkCreateBuffer(g_Device, &bi, g_Allocator, &buf) != VK_SUCCESS) {
                vkFreeMemory(g_Device, stagingMem, g_Allocator); vkDestroyBuffer(g_Device, staging, g_Allocator); return false;
            }
            vkGetBufferMemoryRequirements(g_Device, buf, &mr);
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = RendererUtils::FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(g_Device, &ai, g_Allocator, &mem) != VK_SUCCESS) {
                vkDestroyBuffer(g_Device, buf, g_Allocator); vkFreeMemory(g_Device, stagingMem, g_Allocator); vkDestroyBuffer(g_Device, staging, g_Allocator); return false;
            }
            vkBindBufferMemory(g_Device, buf, mem, 0);
            CopyBuffer(staging, buf, size);
            vkFreeMemory(g_Device, stagingMem, g_Allocator);
            vkDestroyBuffer(g_Device, staging, g_Allocator);
            return true;
        };
        if (!createDev(vertBytes, group.vertexBuffer, group.vertexBufferMemory, vertData.data())) continue;
        if (!createDev(idxCount * sizeof(uint32_t), group.indexBuffer, group.indexBufferMemory, idxData.data())) continue;
    }

    // 3) subMesh → 组映射（-1 = 未合批）
    m_ModelData.subMeshBatchGroup.resize(subs.size(), -1);
    for (size_t i = 0; i < groupOfSub.size(); ++i) {
        if (groupOfSub[i] != SIZE_MAX) m_ModelData.subMeshBatchGroup[i] = (int)groupOfSub[i];
    }
    LOGD("[ModelRenderer] '%s' batch groups=%zu (subMeshes=%zu)", m_ModelData.modelPath.c_str(),
         m_ModelData.batchGroups.size(), subs.size());
}
