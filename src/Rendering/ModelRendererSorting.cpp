#include "ModelRenderer.h"

#include <algorithm>

void ModelRenderer::BuildSortedIndices()
{
    m_ModelData.cachedSortedIndices.clear();
    m_ModelData.cachedSortedIndices.reserve(m_ModelData.subMeshes.size());
    
    for (size_t i = 0; i < m_ModelData.subMeshes.size(); i++) {
        const auto& subMesh = m_ModelData.subMeshes[i];
        const VkBuffer& mainVB = subMesh.vertexBuffer;
        if (mainVB != VK_NULL_HANDLE && 
            subMesh.indexBuffer != VK_NULL_HANDLE && 
            subMesh.indexCount > 0 &&
            subMesh.descriptorSet != VK_NULL_HANDLE) {
            m_ModelData.cachedSortedIndices.push_back(i);
        }
    }
    
    // 按描述符集排序，减少描述符切换次数
    std::sort(m_ModelData.cachedSortedIndices.begin(), m_ModelData.cachedSortedIndices.end(), 
        [this](size_t a, size_t b) {
            return m_ModelData.subMeshes[a].descriptorSet < m_ModelData.subMeshes[b].descriptorSet;
        });
    
    m_ModelData.sortedIndicesDirty = false;
}
