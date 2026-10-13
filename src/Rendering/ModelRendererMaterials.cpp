#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include <array>
#include <unordered_map>
#include "EngineGlobal.h"
#include "Core/ProjectManager.h"
#include "VulkanManager.h"

namespace {
struct SamplerBinding {
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};
// Material descriptors can be shared by submeshes; cache the actual set state,
// not a per-submesh requested state. Accessed on the render thread only.
std::unordered_map<VkDescriptorSet, std::array<SamplerBinding, 4>> samplerBindings;
uint64_t samplerGeneration = 0;
struct ModelSamplerState {
    std::array<int, 6> settings{};
    size_t subMeshCount = 0;
    uint64_t generation = UINT64_MAX;
};
std::unordered_map<const ModelRenderer*, ModelSamplerState> modelSamplerStates;
}

void ModelRendererDetail::InvalidateSamplerBindings(VkDescriptorSet descriptorSet)
{
    samplerBindings.erase(descriptorSet);
    ++samplerGeneration;
}

void ModelRendererDetail::SyncSamplerSettings(ModelRenderer& renderer, const std::array<int, 6>& settings)
{
    auto& state = modelSamplerStates[&renderer];
    const size_t count = renderer.GetSubMeshCount();
    if (state.generation == samplerGeneration && state.subMeshCount == count && state.settings == settings) return;
    for (size_t i = 0; i < count; ++i) {
        for (int type = 0; type < 6; ++type) renderer.UpdateSubMeshSampler(i, type, settings[type]);
    }
    state = {settings, count, samplerGeneration};
}

void ModelRendererDetail::ReleaseSamplerSettings(const ModelRenderer* renderer)
{
    modelSamplerStates.erase(renderer);
}

uint64_t ModelRendererDetail::MaterialBindingGeneration() { return samplerGeneration; }

// ===== 材质/纹理覆盖 =====
void ModelRenderer::UpdateSubMeshSampler(size_t subMeshIndex, int textureType, int samplerType)
{
    if (subMeshIndex >= m_ModelData.subMeshes.size()) {
        return;
    }

    auto& subMesh = m_ModelData.subMeshes[subMeshIndex];
    const std::string* texturePath = nullptr;
    int binding = 0;

    // 根据纹理类型确定纹理路径和绑定位置
    switch (textureType) {
        case 0: // Albedo
            texturePath = &subMesh.diffuseTexturePath;
            binding = 0;
            break;
        case 1: // Normal
            texturePath = &subMesh.normalTexturePath;
            binding = 1;
            break;
        case 2: // Roughness
            texturePath = &subMesh.roughnessTexturePath;
            binding = 2;
            break;
        case 3: // Metallic
            texturePath = &subMesh.metallicTexturePath;
            binding = 3;
            break;
        default:
            return;
    }

    if (texturePath->empty() || !m_TexturePool) {
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
    m_TexturePool->UpdateTextureSampler(*texturePath, newSamplerType);

    // 更新子网格的描述符集
    const TextureInfo* texInfo = m_TexturePool->GetTexture(*texturePath);
    if (texInfo != nullptr && texInfo->imageView != VK_NULL_HANDLE && subMesh.descriptorSet != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texInfo->imageView;
        imageInfo.sampler = m_TexturePool->GetSamplerByType(newSamplerType);

        auto& cached = samplerBindings[subMesh.descriptorSet][binding];
        if (cached.imageView == imageInfo.imageView && cached.sampler == imageInfo.sampler) {
            return;
        }

        VkWriteDescriptorSet descriptorWrite = {};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = subMesh.descriptorSet;
        descriptorWrite.dstBinding = binding;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(g_Device, 1, &descriptorWrite, 0, nullptr);
        cached = {imageInfo.imageView, imageInfo.sampler};
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

    // This writer bypasses the sampler cache; invalidate shared set state.
    ModelRendererDetail::InvalidateSamplerBindings(subMesh.descriptorSet);
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
