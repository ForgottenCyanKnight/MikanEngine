#include "ModelRenderer.h"
#include "Core/Log.h"
#include "Core/VulkanContext.h"
#include "VulkanManager.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

void ModelRenderer::CreateModelBuffers(const MeshData& meshData)
{
    m_ModelData.subMeshes.clear();
    
    for (size_t i = 0; i < meshData.subMeshes.size(); i++) {
        const auto& subMesh = meshData.subMeshes[i];
        
        SubMeshRenderData renderData;
        
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        VkDeviceSize bufferSize = sizeof(Vertex) * subMesh.vertices.size();              // 蒙皮布局 32B/顶点
        
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &stagingBuffer) != VK_SUCCESS) {
            continue;
        }
        
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &memRequirements);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingBufferMemory) != VK_SUCCESS) {
            vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
            continue;
        }
        
        vkBindBufferMemory(g_Device, stagingBuffer, stagingBufferMemory, 0);
        
        void* data;
        vkMapMemory(g_Device, stagingBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, subMesh.vertices.data(), (size_t)bufferSize);
        vkUnmapMemory(g_Device, stagingBufferMemory);
        
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        
        VkBuffer& targetVB = renderData.vertexBuffer;
        VkDeviceMemory& targetVBMem = renderData.vertexBufferMemory;
        if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &targetVB) != VK_SUCCESS) {
            vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
            vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
            continue;
        }
        
        vkGetBufferMemoryRequirements(g_Device, targetVB, &memRequirements);
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &targetVBMem) != VK_SUCCESS) {
            vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
            vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
            vkDestroyBuffer(g_Device, targetVB, g_Allocator);
            continue;
        }
        
        vkBindBufferMemory(g_Device, targetVB, targetVBMem, 0);
        
        CopyBuffer(stagingBuffer, targetVB, bufferSize);
        
        vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        
        bufferSize = sizeof(unsigned int) * subMesh.indices.size();
        
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        
        if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &stagingBuffer) != VK_SUCCESS) {
            continue;
        }
        
        vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &memRequirements);
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingBufferMemory) != VK_SUCCESS) {
            vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
            continue;
        }
        
        vkBindBufferMemory(g_Device, stagingBuffer, stagingBufferMemory, 0);
        
        vkMapMemory(g_Device, stagingBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, subMesh.indices.data(), (size_t)bufferSize);
        vkUnmapMemory(g_Device, stagingBufferMemory);
        
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        
        if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &renderData.indexBuffer) != VK_SUCCESS) {
            vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
            vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
            continue;
        }
        
        vkGetBufferMemoryRequirements(g_Device, renderData.indexBuffer, &memRequirements);
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &renderData.indexBufferMemory) != VK_SUCCESS) {
            vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
            vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
            vkDestroyBuffer(g_Device, renderData.indexBuffer, g_Allocator);
            continue;
        }
        
        vkBindBufferMemory(g_Device, renderData.indexBuffer, renderData.indexBufferMemory, 0);
        
        CopyBuffer(stagingBuffer, renderData.indexBuffer, bufferSize);
        
        vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        
        renderData.vertexCount = static_cast<uint32_t>(subMesh.vertices.size());
        renderData.indexCount = static_cast<uint32_t>(subMesh.indices.size());
        renderData.materialName = subMesh.materialName;
        renderData.name = subMesh.name;
        renderData.metallic = subMesh.metallic;
        renderData.roughness = subMesh.roughness;
        renderData.mrValid = subMesh.hasMRTexture ? 1.0f : 0.0f;
        renderData.alphaMode = subMesh.alphaMode;
        renderData.alphaCutoff = subMesh.alphaCutoff;
        renderData.doubleSided = subMesh.doubleSided;
        renderData.diffuseTransmissionFactor = subMesh.diffuseTransmissionFactor;
        
        // Prefer the source material index. PMX files can legally reuse a
        // material name for different material records, so name-only lookup
        // can assign the first material's descriptor to a later submesh.
        const MaterialTextureInfo* materialInfo = nullptr;
        if (subMesh.materialIndex >= 0 &&
            static_cast<size_t>(subMesh.materialIndex) < meshData.materialTextures.size()) {
            materialInfo = &meshData.materialTextures[static_cast<size_t>(subMesh.materialIndex)];
        } else {
            for (const auto& candidate : meshData.materialTextures) {
                if (candidate.materialName == subMesh.materialName) {
                    materialInfo = &candidate;
                    break;
                }
            }
        }

        if (materialInfo) {
            const auto& material = *materialInfo;
            renderData.diffuseTexturePath = material.diffuseTexturePath;
            renderData.normalTexturePath = material.normalTexturePath;
            renderData.roughnessTexturePath = material.roughnessTexturePath;
            renderData.metallicTexturePath = material.metallicTexturePath;
            renderData.emissiveTexturePath = material.emissiveTexturePath;
            renderData.wrapMode = material.wrapMode;   // 纹理自身环绕（per-texture）
                // subMesh 循环之后，subMesh 直接注入恒为空——此处是实际生效通道）
            if (renderData.alphaMode < 0) renderData.alphaMode = material.alphaMode;
            if (material.alphaMode >= 0) {
                renderData.alphaCutoff = material.alphaCutoff;
            }
                // assimpToGltfMat 恒为 0，会覆盖掉 ModelLoader 按名可靠回填的 1（leaves/wings 双面丢失）。
                // doubleSided 只能取 subMesh（ModelLoader 回填，637 已设）——材质侧不可靠。
            if (renderData.metallic < 0) renderData.metallic = material.metallic;
            if (renderData.roughness < 0) renderData.roughness = material.roughness;
            if (renderData.diffuseTransmissionFactor <= 0.0f)
                renderData.diffuseTransmissionFactor = material.diffuseTransmissionFactor;
        }
        
        // 计算submesh的AABB（位置均在首字段，解码一致）
        if (!subMesh.vertices.empty()) {
            glm::vec3 minBounds(std::numeric_limits<float>::max());
            glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
            
            for (const auto& vertex : subMesh.vertices) {
                minBounds = glm::min(minBounds, vertex.Position);
                maxBounds = glm::max(maxBounds, vertex.Position);
            }
            
            renderData.aabb = AABB(minBounds, maxBounds);
        } else {
            renderData.aabb = AABB();
        }
        
        const VkBuffer& mainVB = renderData.vertexBuffer;
        const VkDeviceMemory& mainVBMem = renderData.vertexBufferMemory;
        if (mainVB != VK_NULL_HANDLE && 
            mainVBMem != VK_NULL_HANDLE && 
            renderData.indexBuffer != VK_NULL_HANDLE && 
            renderData.indexBufferMemory != VK_NULL_HANDLE) {
            m_ModelData.subMeshes.push_back(renderData);
        }
    }
    
    // 子网格数据已变化，标记排序索引需要重建
    m_ModelData.sortedIndicesDirty = true;
    
    CalculateAABB();
}

void ModelRenderer::CalculateAABB()
{
    if (m_MeshData.subMeshes.empty()) {
        m_ModelData.modelMinBounds = glm::vec3(0.0f);
        m_ModelData.modelMaxBounds = glm::vec3(0.0f);
        m_ModelData.modelCenter = glm::vec3(0.0f);
        return;
    }
    
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
    
    for (const auto& subMesh : m_MeshData.subMeshes) {
        for (const auto& vertex : subMesh.vertices) {
            minBounds = glm::min(minBounds, vertex.Position);
            maxBounds = glm::max(maxBounds, vertex.Position);
        }
    }
    
    m_ModelData.modelMinBounds = minBounds;
    m_ModelData.modelMaxBounds = maxBounds;
    m_ModelData.modelCenter = (minBounds + maxBounds) * 0.5f;
}

