#include "ModelRenderer.h"
#include "EngineConfig.h"
#include "Core/RenderGlobals.h"
#include "VulkanManager.h"

#include <array>
#include <cstdio>

static SamplerType ModelTextureSampler(int wrapMode = 10497) {
    if (!g_ModelMipmap) return SamplerType::LinearNoMip;
    return wrapMode == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ? SamplerType::LinearClamp : SamplerType::Linear;
}

// 蒙皮 UBO 3 帧槽动态偏移（dynamic UBO——动画更新处设值，10 处 descriptor 绑定读取；无动画模型恒 0）

void ModelRenderer::SetupDescriptorSets()
{
    if (m_ModelData.subMeshes.empty()) {
        return;
    }
    
    for (auto& subMesh : m_ModelData.subMeshes) {
        // 创建材质哈希
        MaterialHash materialHash;
        materialHash.diffuseTexturePath = subMesh.diffuseTexturePath;
        materialHash.normalTexturePath = subMesh.normalTexturePath;
        materialHash.roughnessTexturePath = subMesh.roughnessTexturePath;
        materialHash.metallicTexturePath = subMesh.metallicTexturePath;
    materialHash.emissiveTexturePath = subMesh.emissiveTexturePath;
        
        // 从缓存中获取或创建描述符集（静态模型 → 静态 layout 无 binding 4，shader 反射与 layout 严格匹配，RenderDoc 回放兼容）
        auto materialCallback = [this, &subMesh](VkDescriptorSet descriptorSet) {
                // 更新描述符集的回调函数
                std::array<VkWriteDescriptorSet, 6> writes = {};
                std::array<VkDescriptorImageInfo, 5> imageInfos = {};
                VkDescriptorBufferInfo boneBufInfo = {};   // binding 4 骨骼矩阵 UBO（声明在函数体级：写入数组 pBufferInfo 指向它，必须在 vkUpdateDescriptorSets 前保持有效）
                uint32_t writeCount = 0;

                VkDescriptorImageInfo whiteInfo = {};
                if (m_TexturePool) {
                    m_TexturePool->LoadTexture2D("white", EngineConfig::GetEngineTexturePath("material.png"));
                    const TextureInfo* whiteTex = m_TexturePool->GetTexture("white");
                    if (whiteTex != nullptr && whiteTex->imageView != VK_NULL_HANDLE) {
                        whiteInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        whiteInfo.imageView = whiteTex->imageView;
                        whiteInfo.sampler = m_TexturePool->GetSampler("white");
                    }
                }

                // （white 占位会 albedo += 1.0 全白——Bistro 模型级 hasEmissive 使所有 subMesh 采样 binding 5）
                VkDescriptorImageInfo blackInfo = {};
                if (m_TexturePool) {
                    m_TexturePool->LoadTexture2D("black", EngineConfig::GetEngineTexturePath("black.png"));
                    const TextureInfo* blackTex = m_TexturePool->GetTexture("black");
                    if (blackTex != nullptr && blackTex->imageView != VK_NULL_HANDLE) {
                        blackInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        blackInfo.imageView = blackTex->imageView;
                        blackInfo.sampler = m_TexturePool->GetSampler("black");
                    }
                }
                
                // 加载模型纹理并设置描述符
                bool wroteDiffuse = false;
                if (!subMesh.diffuseTexturePath.empty() && m_TexturePool) {
                    m_TexturePool->LoadTexture2D(subMesh.diffuseTexturePath, subMesh.diffuseTexturePath,
                        ModelTextureSampler(subMesh.wrapMode));
                    const TextureInfo* diffuseTex = m_TexturePool->GetTexture(subMesh.diffuseTexturePath);
                    if (diffuseTex != nullptr && diffuseTex->imageView != VK_NULL_HANDLE) {
                        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos[writeCount].imageView = diffuseTex->imageView;
                        imageInfos[writeCount].sampler = m_TexturePool->GetSampler(subMesh.diffuseTexturePath);
                        
                        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[writeCount].dstSet = descriptorSet;
                        writes[writeCount].dstBinding = 0;
                        writes[writeCount].dstArrayElement = 0;
                        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[writeCount].descriptorCount = 1;
                        writes[writeCount].pImageInfo = &imageInfos[writeCount];
                        writeCount++;
                        wroteDiffuse = true;
                    }
                }
                if (!wroteDiffuse && whiteInfo.imageView != VK_NULL_HANDLE) {
                    imageInfos[writeCount] = whiteInfo;
                    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[writeCount].dstSet = descriptorSet;
                    writes[writeCount].dstBinding = 0;
                    writes[writeCount].dstArrayElement = 0;
                    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[writeCount].descriptorCount = 1;
                    writes[writeCount].pImageInfo = &imageInfos[writeCount];
                    writeCount++;
                }
                
                if (!subMesh.normalTexturePath.empty() && m_TexturePool) {
                    m_TexturePool->LoadTexture2D(subMesh.normalTexturePath, subMesh.normalTexturePath,
                        ModelTextureSampler(subMesh.wrapMode));
                    const TextureInfo* normalTex = m_TexturePool->GetTexture(subMesh.normalTexturePath);
                    if (normalTex != nullptr && normalTex->imageView != VK_NULL_HANDLE) {
                        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos[writeCount].imageView = normalTex->imageView;
                        imageInfos[writeCount].sampler = m_TexturePool->GetSampler(subMesh.normalTexturePath);
                        
                        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[writeCount].dstSet = descriptorSet;
                        writes[writeCount].dstBinding = 1;
                        writes[writeCount].dstArrayElement = 0;
                        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[writeCount].descriptorCount = 1;
                        writes[writeCount].pImageInfo = &imageInfos[writeCount];
                        writeCount++;
                    }
                }
                
                // 黑占位 = roughness 0 / metallic 0（中性）——与"回退默认参数"语义一致；
                // 同时 mrValid=0 标记（下方 fallback 分支）→ shader 跳过采样走默认参数（与加载阶段 hasMRTexture 双保险）
                const TextureInfo* fallbackTex = m_TexturePool ? m_TexturePool->GetTexture("black") : nullptr;
                VkSampler fallbackSampler = m_TexturePool ? m_TexturePool->GetSamplerByType(SamplerType::Linear) : VK_NULL_HANDLE;
                if (!subMesh.roughnessTexturePath.empty() && m_TexturePool) {
                    m_TexturePool->LoadTexture2D(subMesh.roughnessTexturePath, subMesh.roughnessTexturePath,
                        ModelTextureSampler(subMesh.wrapMode));
                    const TextureInfo* roughnessTex = m_TexturePool->GetTexture(subMesh.roughnessTexturePath);
                    if (roughnessTex != nullptr && roughnessTex->imageView != VK_NULL_HANDLE) {
                        // （如 DamagedHelmet 面罩）的 MR 纹理整体偏黑是合法数据，判黑会误伤回退成粗糙非金属。
                        // MR 有效性由加载阶段 hasMRTexture（纹理引用存在与否）决定。
                        subMesh.mrValid = 1.0f;
                        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos[writeCount].imageView = roughnessTex->imageView;
                        imageInfos[writeCount].sampler = m_TexturePool->GetSampler(subMesh.roughnessTexturePath);
                        
                        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[writeCount].dstSet = descriptorSet;
                        writes[writeCount].dstBinding = 2;
                        writes[writeCount].dstArrayElement = 0;
                        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[writeCount].descriptorCount = 1;
                        writes[writeCount].pImageInfo = &imageInfos[writeCount];
                        writeCount++;
                    } else {
                        subMesh.mrValid = 0.0f;
                        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos[writeCount].imageView = fallbackTex ? fallbackTex->imageView : VK_NULL_HANDLE;
                        imageInfos[writeCount].sampler = fallbackSampler;
                        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[writeCount].dstSet = descriptorSet;
                        writes[writeCount].dstBinding = 2;
                        writes[writeCount].dstArrayElement = 0;
                        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[writeCount].descriptorCount = 1;
                        writes[writeCount].pImageInfo = &imageInfos[writeCount];
                        writeCount++;
                    }
                } else if (fallbackTex != nullptr && fallbackTex->imageView != VK_NULL_HANDLE) {
                    subMesh.mrValid = 0.0f;
                    imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    imageInfos[writeCount].imageView = fallbackTex->imageView;
                    imageInfos[writeCount].sampler = fallbackSampler;
                    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[writeCount].dstSet = descriptorSet;
                    writes[writeCount].dstBinding = 2;
                    writes[writeCount].dstArrayElement = 0;
                    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[writeCount].descriptorCount = 1;
                    writes[writeCount].pImageInfo = &imageInfos[writeCount];
                    writeCount++;
                }
                
                if (!subMesh.metallicTexturePath.empty() && m_TexturePool) {
                    m_TexturePool->LoadTexture2D(subMesh.metallicTexturePath, subMesh.metallicTexturePath,
                        ModelTextureSampler(subMesh.wrapMode));
                    const TextureInfo* metallicTex = m_TexturePool->GetTexture(subMesh.metallicTexturePath);
                    if (metallicTex != nullptr && metallicTex->imageView != VK_NULL_HANDLE) {
                        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos[writeCount].imageView = metallicTex->imageView;
                        imageInfos[writeCount].sampler = m_TexturePool->GetSampler(subMesh.metallicTexturePath);
                        
                        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[writeCount].dstSet = descriptorSet;
                        writes[writeCount].dstBinding = 3;
                        writes[writeCount].dstArrayElement = 0;
                        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[writeCount].descriptorCount = 1;
                        writes[writeCount].pImageInfo = &imageInfos[writeCount];
                        writeCount++;
                    } else {
                        subMesh.mrValid = 0.0f;
                        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos[writeCount].imageView = fallbackTex ? fallbackTex->imageView : VK_NULL_HANDLE;
                        imageInfos[writeCount].sampler = fallbackSampler;
                        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[writeCount].dstSet = descriptorSet;
                        writes[writeCount].dstBinding = 3;
                        writes[writeCount].dstArrayElement = 0;
                        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[writeCount].descriptorCount = 1;
                        writes[writeCount].pImageInfo = &imageInfos[writeCount];
                        writeCount++;
                    }
                } else if (!subMesh.roughnessTexturePath.empty() && m_TexturePool) {
                    // （glTF metallicRoughnessTexture 是单纹理：B=metallic、G=roughness——ModelLoader 只解析
                    // roughness 路径；旧 fallback 写 black 占位 → metallic 恒 0 → 头盔/材质球金属度丢失
                    // "被误判标记成默认材质"）
                    m_TexturePool->LoadTexture2D(subMesh.roughnessTexturePath, subMesh.roughnessTexturePath,
                        ModelTextureSampler(subMesh.wrapMode));
                    const TextureInfo* mrTex = m_TexturePool->GetTexture(subMesh.roughnessTexturePath);
                    if (mrTex != nullptr && mrTex->imageView != VK_NULL_HANDLE) {
                        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos[writeCount].imageView = mrTex->imageView;
                        imageInfos[writeCount].sampler = m_TexturePool->GetSampler(subMesh.roughnessTexturePath);
                        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[writeCount].dstSet = descriptorSet;
                        writes[writeCount].dstBinding = 3;
                        writes[writeCount].dstArrayElement = 0;
                        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[writeCount].descriptorCount = 1;
                        writes[writeCount].pImageInfo = &imageInfos[writeCount];
                        writeCount++;
                    }
                } else if (fallbackTex != nullptr && fallbackTex->imageView != VK_NULL_HANDLE) {
                    subMesh.mrValid = 0.0f;
                    imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    imageInfos[writeCount].imageView = fallbackTex->imageView;
                    imageInfos[writeCount].sampler = fallbackSampler;
                    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[writeCount].dstSet = descriptorSet;
                    writes[writeCount].dstBinding = 3;
                    writes[writeCount].dstArrayElement = 0;
                    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[writeCount].descriptorCount = 1;
                    writes[writeCount].pImageInfo = &imageInfos[writeCount];
                    writeCount++;
                }

                if (!subMesh.emissiveTexturePath.empty() && m_TexturePool) {
                    m_TexturePool->LoadTexture2D(subMesh.emissiveTexturePath, subMesh.emissiveTexturePath,
                        ModelTextureSampler(subMesh.wrapMode));
                    const TextureInfo* emissiveTex = m_TexturePool->GetTexture(subMesh.emissiveTexturePath);
                    if (emissiveTex != nullptr && emissiveTex->imageView != VK_NULL_HANDLE) {
                        imageInfos[writeCount].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos[writeCount].imageView = emissiveTex->imageView;
                        imageInfos[writeCount].sampler = m_TexturePool->GetSampler(subMesh.emissiveTexturePath);
                        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        writes[writeCount].dstSet = descriptorSet;
                        writes[writeCount].dstBinding = 5;
                        writes[writeCount].dstArrayElement = 0;
                        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[writeCount].descriptorCount = 1;
                        writes[writeCount].pImageInfo = &imageInfos[writeCount];
                        writeCount++;
                    }
                }
                if (writeCount > 0 && writeCount < 6 && blackInfo.imageView != VK_NULL_HANDLE &&
                    subMesh.emissiveTexturePath.empty()) {
                    // 无 emissive：black 占位保证 binding 5 始终有效（emissive += 0 无影响）
                    imageInfos[writeCount] = blackInfo;
                    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[writeCount].dstSet = descriptorSet;
                    writes[writeCount].dstBinding = 5;
                    writes[writeCount].dstArrayElement = 0;
                    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[writeCount].descriptorCount = 1;
                    writes[writeCount].pImageInfo = &imageInfos[writeCount];
                    writeCount++;
                }

                // binding 4: 骨骼蒙皮矩阵 UBO（固定 256）—— 全部模型统一蒙皮布局，无条件写入
                // 无纹理模型（如 glb 内嵌纹理加载失败）sampler 写入可能为 0，
                // 若也不写，descriptor set 缺 binding 4，蒙皮 shader 静态使用该绑定 → draw 无效 → 模型不可见。
                {
                    boneBufInfo = {};
                    boneBufInfo.buffer = m_ModelData.uniformBuffer;
                    boneBufInfo.offset = 0;
                    boneBufInfo.range = MAX_BONES * sizeof(glm::mat4);
                    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[writeCount].dstSet = descriptorSet;
                    writes[writeCount].dstBinding = 4;
                    writes[writeCount].dstArrayElement = 0;
                    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                    writes[writeCount].descriptorCount = 1;
                    writes[writeCount].pBufferInfo = &boneBufInfo;
                    writeCount++;
                }

                // [diag-20260806] 确认材质回调执行 + binding 4 蒙皮 buffer 写入（编辑器日志可见；no resource 排查用）
                static bool s_dsDiag = true;
                if (s_dsDiag) {
                    s_dsDiag = false;
                    bool b4written = false;
                    for (uint32_t i = 0; i < writeCount; ++i)
                        if (writes[i].dstBinding == 4) b4written = true;
                    printf("[diag] descriptor callback: writes=%u b4=%s buf=%p range=%llu\n",
                        writeCount, b4written ? "WRITTEN" : "MISSING",
                        (void*)boneBufInfo.buffer,
                        (unsigned long long)boneBufInfo.range);
                    fflush(stdout);
                }

                vkUpdateDescriptorSets(g_Device, writeCount, writes.data(), 0, nullptr);
            };

        subMesh.descriptorSet = DescriptorSetCache::GetInstance().GetOrCreate(materialHash, materialCallback);
    }
    
    // 构建缓存的排序索引，按描述符集分组以减少渲染时的切换
    BuildSortedIndices();
}
