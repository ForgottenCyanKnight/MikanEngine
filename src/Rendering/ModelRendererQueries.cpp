#include "ModelRenderer.h"

// ===== 只读查询 =====
bool ModelRenderer::HasAlbedoTexture() const
{
    for (const auto& subMesh : m_ModelData.subMeshes) {
        if (!subMesh.diffuseTexturePath.empty()) {
            return true;
        }
    }
    return false;
}

bool ModelRenderer::HasNormalTexture() const
{
    for (const auto& subMesh : m_ModelData.subMeshes) {
        if (!subMesh.normalTexturePath.empty()) {
            return true;
        }
    }
    return false;
}

bool ModelRenderer::HasEmissiveTexture() const
{
    for (const auto& subMesh : m_ModelData.subMeshes) {
        if (!subMesh.emissiveTexturePath.empty()) {
            return true;
        }
    }
    return false;
}

bool ModelRenderer::HasRoughnessTexture() const
{
    for (const auto& mt : m_MeshData.materialTextures) {
        if (mt.hasRoughnessTexture) return true;
    }
    return false;
}

bool ModelRenderer::HasMetallicTexture() const
{
    for (const auto& mt : m_MeshData.materialTextures) {
        if (mt.hasMetallicTexture) return true;
    }
    return false;
}

bool ModelRenderer::HasDoubleSided() const
{
    for (const auto& sm : m_ModelData.subMeshes) {
        if (sm.doubleSided) return true;
    }
    return false;
}

const std::string& ModelRenderer::GetAlbedoTexturePath() const
{
    static const std::string empty;
    for (const auto& subMesh : m_ModelData.subMeshes) {
        if (!subMesh.diffuseTexturePath.empty()) {
            return subMesh.diffuseTexturePath;
        }
    }
    return empty;
}

AABB ModelRenderer::GetAABB() const
{
    return AABB(m_ModelData.modelMinBounds, m_ModelData.modelMaxBounds);
}

std::vector<AABB> ModelRenderer::GetSubMeshAABBs() const
{
    std::vector<AABB> subMeshAABBs;
    subMeshAABBs.reserve(m_ModelData.subMeshes.size());

    for (const auto& subMesh : m_ModelData.subMeshes) {
        subMeshAABBs.push_back(subMesh.aabb);
    }

    return subMeshAABBs;
}

size_t ModelRenderer::GetVertexCount() const
{
    size_t count = 0;
    for (const auto& subMesh : m_MeshData.subMeshes) {
        count += subMesh.vertices.size();
    }
    return count;
}

size_t ModelRenderer::GetSubMeshCount() const
{
    return m_MeshData.subMeshes.size();
}

std::vector<AABB> ModelRenderer::GetBVHNodeBounds() const
{
    return m_BVHData.GetAllNodeBounds();
}

std::vector<AABB> ModelRenderer::GetBVHNodeBounds(size_t subMeshIndex) const
{
    return m_BVHData.GetAllNodeBounds(subMeshIndex);
}

std::vector<AABB> ModelRenderer::GetTopLevelBVHNodeBounds() const
{
    return m_BVHData.GetTopLevelNodeBounds();
}
