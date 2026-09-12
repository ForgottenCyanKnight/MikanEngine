#include "ModelRenderer.h"
#include "EngineGlobal.h"
#include "Core/ProjectManager.h"
#include "VulkanManager.h"

// ===== 材质/纹理覆盖 =====
void ModelRenderer::UpdateSubMeshSampler(size_t subMeshIndex, int textureType, int samplerType)
{
    if (subMeshIndex >= m_ModelData.subMeshes.size()) {
        return;
    }

    auto& subMesh = m_ModelData.subMeshes[subMeshIndex];
    std::string texturePath;
    int binding = 0;

    // 根据纹理类型确定纹理路径和绑定位置
    switch (textureType) {
        case 0: // Albedo
            texturePath = subMesh.diffuseTexturePath;
            binding = 0;
            break;
        case 1: // Normal
            texturePath = subMesh.normalTexturePath;
            binding = 1;
            break;
        case 2: // Roughness
            texturePath = subMesh.roughnessTexturePath;
            binding = 2;
            break;
        case 3: // Metallic
            texturePath = subMesh.metallicTexturePath;
            binding = 3;
            break;
        default:
            return;
    }

    if (texturePath.empty() || !m_TexturePool) {
        return;
    }

    // 转换采样器类型
    SamplerType newSamplerType;
    switch (samplerType) {
        case 0: newSamplerType = SamplerType::Linear; break;
        case 1: newSamplerType = SamplerType::Nearest; break;
        case 2: newSamplerType = SamplerType::LinearClamp; break;
        case 3: newSamplerType = SamplerType::NearestClamp; break;
        default: newSamplerType = SamplerType::Linear; break;
    }

    // 更新纹理池中的采样器类型
    m_TexturePool->UpdateTextureSampler(texturePath, newSamplerType);

    // 更新子网格的描述符集
    const TextureInfo* texInfo = m_TexturePool->GetTexture(texturePath);
    if (texInfo != nullptr && texInfo->imageView != VK_NULL_HANDLE && subMesh.descriptorSet != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texInfo->imageView;
        imageInfo.sampler = m_TexturePool->GetSamplerByType(newSamplerType);

        VkWriteDescriptorSet descriptorWrite = {};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = subMesh.descriptorSet;
        descriptorWrite.dstBinding = binding;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(g_Device, 1, &descriptorWrite, 0, nullptr);
    }
}

// textureType: 0=Albedo 1=Normal 2=Roughness 3=Metallic；samplerType: 0=Linear 1=Nearest 2=LinearClamp 3=NearestClamp。
// subMeshIndex < 0 或越界 = 全部 subMesh（旧行为）。
void ModelRenderer::ApplyTextureToSubMesh(int subMeshIndex, int textureType, const std::string& path, int samplerType)
{
    SamplerType newSamplerType;
    switch (samplerType) {
        case 0: newSamplerType = SamplerType::Linear; break;
        case 1: newSamplerType = SamplerType::Nearest; break;
        case 2: newSamplerType = SamplerType::LinearClamp; break;
        case 3: newSamplerType = SamplerType::NearestClamp; break;
        default: newSamplerType = SamplerType::Linear; break;
    }

    const std::string resolved = path.empty() ? std::string() :
        ProjectManager::GetInstance().ResolveAssetPath(path);

    if (subMeshIndex < 0 || subMeshIndex >= (int)m_ModelData.subMeshes.size()) {
        for (auto& subMesh : m_ModelData.subMeshes) {
            ApplyTextureToSubMesh(static_cast<int>(&subMesh - m_ModelData.subMeshes.data()), textureType, path, samplerType);
        }
        return;
    }

    auto& subMesh = m_ModelData.subMeshes[subMeshIndex];
    std::string* slot = nullptr;
    int binding = 0;
    switch (textureType) {
        case 0: slot = &subMesh.diffuseTexturePath;   binding = 0; break;
        case 1: slot = &subMesh.normalTexturePath;    binding = 1; break;
        case 2: slot = &subMesh.roughnessTexturePath; binding = 2; break;
        case 3: slot = &subMesh.metallicTexturePath;  binding = 3; break;
        default: return;
    }
    if (!resolved.empty()) *slot = resolved;
    if (slot->empty() || !m_TexturePool) return;

    // 重新加载纹理（同名 key=绝对路径）并更新采样器
    m_TexturePool->LoadTexture2D(*slot, *slot, newSamplerType);
    m_TexturePool->UpdateTextureSampler(*slot, newSamplerType);

    // 重绑 descriptor（与 UpdateSubMeshSampler 同逻辑）
    const TextureInfo* texInfo = m_TexturePool->GetTexture(*slot);
    if (texInfo != nullptr && texInfo->imageView != VK_NULL_HANDLE && subMesh.descriptorSet != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texInfo->imageView;
        imageInfo.sampler = m_TexturePool->GetSamplerByType(newSamplerType);

        VkWriteDescriptorSet descriptorWrite = {};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = subMesh.descriptorSet;
        descriptorWrite.dstBinding = binding;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(g_Device, 1, &descriptorWrite, 0, nullptr);
    }
    if (subMeshIndex >= 0 && subMeshIndex < (int)m_ModelData.subMeshBatchGroup.size()) {
        m_ModelData.subMeshBatchGroup[subMeshIndex] = -1;
    }
}

void ModelRenderer::ApplyTextureToAllSubMeshes(int textureType, const std::string& path, int samplerType)
{
    ApplyTextureToSubMesh(-1, textureType, path, samplerType);
}
