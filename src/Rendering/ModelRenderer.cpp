#include "ModelRenderer.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "Core/Log.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanContext.h"

#include "VulkanManager.h"
#include <map>
#include <set>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3_image/SDL_image.h>
#include <vector>
#include <cstring>
#include <iostream>
#include <limits>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

// -1 = 未设（无 glTF factor）→ model.frag 用实例 materialData（材质组件）或引擎默认
// forceDoubleSided 是当前绘制管线的 ECS 双面开关；不能只依赖 sm.doubleSided，
// 因为编辑器创建的普通平面也可以通过 RenderComponent 单独开启双面渲染。
static void PushSubMeshMaterialParams(VkCommandBuffer cmd, VkPipelineLayout layout,
                                      const SubMeshRenderData& sm, bool forceDoubleSided = false) {
    glm::vec4 mat(sm.metallic >= 0.0f ? sm.metallic : -1.0f,
                  sm.roughness >= 0.0f ? sm.roughness : -1.0f,
                  sm.ao, sm.mrValid);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData), 16, &mat);
    glm::vec4 alpha(sm.alphaCutoff, (float)sm.alphaMode,
                    (forceDoubleSided || sm.doubleSided) ? 1.0f : 0.0f,
                    sm.diffuseTransmissionFactor);   // z=doubleSided，w=显式漫反射透射系数
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData) + 16, 16, &alpha);
}

static uint64_t SubMeshFNV1a64(const void* data, size_t size, uint64_t h) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
static uint64_t ComputeGeometryHash(const void* verts, size_t vertCount, size_t vertSize, const std::vector<uint32_t>& indices) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t* p = (const uint8_t*)verts;
    for (size_t i = 0; i < vertCount; i++) h = SubMeshFNV1a64(p + i * vertSize, vertSize, h);
    h = SubMeshFNV1a64(indices.data(), indices.size() * sizeof(indices[0]), h);
    return h;
}

// 模型纹理采样器选择：mip 开关 × 纹理自身环绕（per-texture，来自 gltf sampler / 默认 REPEAT）
// - 关 mip：LinearNoMip（REPEAT，临时调试用）
// - 开 mip + REPEAT：Linear（平铺纹理）
// - 开 mip + CLAMP_TO_EDGE：LinearClamp（UV 钳制消除边缘接缝）
static SamplerType ModelTextureSampler(int wrapMode = 10497) {
    if (!g_ModelMipmap) return SamplerType::LinearNoMip;
    return wrapMode == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ? SamplerType::LinearClamp : SamplerType::Linear;
}

// 蒙皮 UBO 3 帧槽动态偏移（dynamic UBO——动画更新处设值，10 处 descriptor 绑定读取；无动画模型恒 0）
static uint32_t g_BoneDynamicOffset = 0;

// 实例上传池故意放在 ModelRenderer.cpp，而不是 ModelRenderData 内。
// ModelRenderData 会跨多个渲染模块使用；把 std::vector 嵌进去会扩大并改变
// 其布局，旧模块/旧对象生命周期一旦混用就可能把后面的排序 vector 写坏。
// 这里的池只按 ModelRenderer 实例索引，生命周期由 Cleanup 显式回收。
namespace {
struct InstanceUploadSlot {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    size_t capacity = 0;
};

struct InstanceUploadState {
    std::vector<InstanceUploadSlot> slots[ModelRenderData::MAX_FRAMES_IN_FLIGHT];
    uint32_t cursor[ModelRenderData::MAX_FRAMES_IN_FLIGHT] = {};
    uint64_t frameSerial = UINT64_MAX;
};

std::unordered_map<const ModelRenderer*, InstanceUploadState> g_InstanceUploadStates;

void DestroyInstanceUploadSlot(InstanceUploadSlot& slot)
{
    if (slot.mapped != nullptr && slot.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(g_Device, slot.memory);
    }
    if (slot.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, slot.buffer, g_Allocator);
    }
    if (slot.memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, slot.memory, g_Allocator);
    }
    slot = {};
}

void DestroyInstanceUploadState(InstanceUploadState& state)
{
    for (auto& bucket : state.slots) {
        for (auto& slot : bucket) {
            DestroyInstanceUploadSlot(slot);
        }
        bucket.clear();
    }
    for (uint32_t& cursor : state.cursor) {
        cursor = 0;
    }
    state.frameSerial = UINT64_MAX;
}

bool CreateInstanceUploadSlot(size_t capacity, InstanceUploadSlot& slot)
{
    const VkDeviceSize bufferSize = sizeof(ModelInstanceData) * capacity;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &slot.buffer) != VK_SUCCESS) {
        slot = {};
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(g_Device, slot.buffer, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &slot.memory) != VK_SUCCESS) {
        DestroyInstanceUploadSlot(slot);
        return false;
    }
    if (vkBindBufferMemory(g_Device, slot.buffer, slot.memory, 0) != VK_SUCCESS) {
        DestroyInstanceUploadSlot(slot);
        return false;
    }
    if (vkMapMemory(g_Device, slot.memory, 0, bufferSize, 0, &slot.mapped) != VK_SUCCESS) {
        DestroyInstanceUploadSlot(slot);
        return false;
    }
    slot.capacity = capacity;
    return true;
}
}

static std::string SubMeshMaterialKey(const SubMeshRenderData& sm) {
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

ModelRenderer::ModelRenderer()
    : m_TexturePool(nullptr)
{
}

ModelRenderer::~ModelRenderer()
{
}

void ModelRenderer::Cleanup()
{
    DestroyBatchGroups();
    for (auto& subMesh : m_ModelData.subMeshes) {
        if (subMesh.vertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, subMesh.vertexBuffer, g_Allocator);
        }
        if (subMesh.vertexBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, subMesh.vertexBufferMemory, g_Allocator);
        }
        if (subMesh.indexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, subMesh.indexBuffer, g_Allocator);
        }
        if (subMesh.indexBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, subMesh.indexBufferMemory, g_Allocator);
        }
        // 描述符现在由全局缓存管理，不需要单独清理
        subMesh.descriptorSet = VK_NULL_HANDLE;
    }
    m_ModelData.subMeshes.clear();
    
    if (m_ModelData.uniformBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_ModelData.uniformBuffer, g_Allocator);
    }
    if (m_ModelData.uniformBufferMemory != VK_NULL_HANDLE) {
        if (m_ModelData.boneBufferMapped != nullptr) {
            vkUnmapMemory(g_Device, m_ModelData.uniformBufferMemory);
            m_ModelData.boneBufferMapped = nullptr;
        }
        vkFreeMemory(g_Device, m_ModelData.uniformBufferMemory, g_Allocator);
    }

    // CPU 蒙皮顶点缓冲
    if (m_ModelData.skinnedVertexBufferMemory != VK_NULL_HANDLE) {
        if (m_ModelData.skinnedBufferMapped != nullptr) {
            vkUnmapMemory(g_Device, m_ModelData.skinnedVertexBufferMemory);
            m_ModelData.skinnedBufferMapped = nullptr;
        }
        vkFreeMemory(g_Device, m_ModelData.skinnedVertexBufferMemory, g_Allocator);
    }
    if (m_ModelData.skinnedVertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_ModelData.skinnedVertexBuffer, g_Allocator);
        m_ModelData.skinnedVertexBuffer = VK_NULL_HANDLE;
    }
    if (m_ModelData.boneBufferView != VK_NULL_HANDLE) {
        vkDestroyBufferView(g_Device, m_ModelData.boneBufferView, g_Allocator);
        m_ModelData.boneBufferView = VK_NULL_HANDLE;
    }
    m_ModelData.skinnedSubMeshes.clear();
    m_ModelData.skinnedBufferOffsets.clear();
    
    if (const auto it = g_InstanceUploadStates.find(this); it != g_InstanceUploadStates.end()) {
        DestroyInstanceUploadState(it->second);
        g_InstanceUploadStates.erase(it);
    }
    // 兼容旧的固定槽字段：当前实际上传由外置池持有，这些字段保持为空；
    // 若旧生命周期留下资源，也在这里安全释放，避免重复句柄。
    for (size_t frame = 0; frame < ModelRenderData::MAX_FRAMES_IN_FLIGHT; ++frame) {
        if (m_ModelData.instanceBufferMapped[frame] != nullptr &&
            m_ModelData.instanceBufferMemories[frame] != VK_NULL_HANDLE) {
            vkUnmapMemory(g_Device, m_ModelData.instanceBufferMemories[frame]);
            m_ModelData.instanceBufferMapped[frame] = nullptr;
        }
        if (m_ModelData.instanceBuffers[frame] != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, m_ModelData.instanceBuffers[frame], g_Allocator);
            m_ModelData.instanceBuffers[frame] = VK_NULL_HANDLE;
        }
        if (m_ModelData.instanceBufferMemories[frame] != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, m_ModelData.instanceBufferMemories[frame], g_Allocator);
            m_ModelData.instanceBufferMemories[frame] = VK_NULL_HANDLE;
        }
    }
    m_ModelData.instanceBuffer = VK_NULL_HANDLE;
    m_ModelData.instanceBufferMemory = VK_NULL_HANDLE;
    
    m_ModelData.pipeline.Cleanup();
    m_ModelData.doubleSidedPipeline.Cleanup();  // 清理双面渲染管线
    m_ModelData.wireframePipeline.Cleanup();  // 清理线框渲染管线
    
    BaseRenderer::Cleanup();
}

void ModelRenderer::Init(VkRenderPass renderPass)
{
    m_TexturePool = g_TexturePool;
    if (m_TexturePool == nullptr) {
        return;
    }
    
    CreatePipeline(renderPass);
    CreateUniformBuffer();
    CreateInstanceBuffer(4096);
}

void ModelRenderer::LoadModel(const std::string& path)
{
    m_ModelData.modelPath = path;

    // 材质 descriptor 缓存失效：每次加载模型重建材质 set。
    // 原因：DescriptorSetCache 单例跨模型存活；蒙皮 binding 4 类型演进（texel→UBO→SSBO）后，
    // 旧 set 被缓存命中则回调不执行 → shader 引用空/旧类型 binding → RenderDoc 显示 no resource → 不渲染。
    DescriptorSetCache::GetInstance().ClearCache();

    ModelLoadResult result = ModelLoader::LoadModelWithTextures(path);
    m_MeshData = result.meshData;
    CreateModelBuffers(m_MeshData);
    SetupDescriptorSets();
    RebuildBatchGroups();
    // 几何统计（LOGD——排查资产规模用）
    {
        size_t tv = 0, ti = 0;
        for (const auto& sm : m_MeshData.subMeshes) {
            tv += sm.vertices.size();
            ti += sm.indices.size();
        }
        LOGD("[ModelRenderer] '%s' subMeshes=%zu verts=%zu tris=%zu",
            m_ModelData.modelPath.c_str(), m_MeshData.subMeshes.size(), tv, ti / 3);
    }

    // ===== 骨骼动画初始化 =====
    m_ModelData.hasAnimation = !m_MeshData.animations.empty();
    m_ModelData.boneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    m_ModelData.currentClip = 0;
    m_ModelData.animTime = 0.0f;
    m_ModelData.animSpeed = 1.0f;
    m_ModelData.animLoop = true;
    m_ModelData.animPlaying = m_ModelData.hasAnimation;
    // PMX commonly contains a bind pose without an embedded VMD clip. A
    // model still needs its bone matrices and CPU fallback in that case.
    m_ModelData.hasSkinning = !m_MeshData.bones.empty();
    if (m_ModelData.hasAnimation || m_ModelData.hasSkinning) {
        // 初始化为绑定姿势蒙皮矩阵并写入 UBO
        for (size_t i = 0; i < m_MeshData.bones.size() && i < MAX_BONES; i++) {
            m_ModelData.boneMatrices[i] = m_MeshData.bones[i].globalTransform * m_MeshData.bones[i].offsetMatrix;
        }
        if (m_ModelData.boneBufferMapped) {
            memcpy(m_ModelData.boneBufferMapped, m_ModelData.boneMatrices.data(), MAX_BONES * sizeof(glm::mat4));
        }

        // ===== CPU 蒙皮缓冲创建（有骨骼模型：host-visible 顶点缓冲，每帧上传蒙皮结果）=====
        if (m_ModelData.hasSkinning) {
            m_ModelData.skinnedSubMeshes.resize(m_MeshData.subMeshes.size());
            size_t totalVerts = 0;
            for (size_t s = 0; s < m_MeshData.subMeshes.size(); s++) {
                m_ModelData.skinnedBufferOffsets.push_back(totalVerts * sizeof(Vertex));
                totalVerts += m_MeshData.subMeshes[s].vertices.size();
            }
            const VkDeviceSize bufSize = totalVerts * sizeof(Vertex);
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = bufSize;
            bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(g_Device, &bi, g_Allocator, &m_ModelData.skinnedVertexBuffer) == VK_SUCCESS) {
                VkMemoryRequirements mr;
                vkGetBufferMemoryRequirements(g_Device, m_ModelData.skinnedVertexBuffer, &mr);
                VkMemoryAllocateInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                ai.allocationSize = mr.size;
                ai.memoryTypeIndex = RendererUtils::FindMemoryType(mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (vkAllocateMemory(g_Device, &ai, g_Allocator, &m_ModelData.skinnedVertexBufferMemory) == VK_SUCCESS) {
                    vkBindBufferMemory(g_Device, m_ModelData.skinnedVertexBuffer, m_ModelData.skinnedVertexBufferMemory, 0);
                    vkMapMemory(g_Device, m_ModelData.skinnedVertexBufferMemory, 0, bufSize, 0, &m_ModelData.skinnedBufferMapped);
                }
            }
            // 初始绑定姿势蒙皮（CPU 蒙皮，写入缓冲）
            UpdateAnimation(0.0f);
        }
        printf("[ModelRenderer] '%s' has %zu bones, %zu animation clip(s) - auto play\n",
            path.c_str(), m_MeshData.bones.size(), m_MeshData.animations.size());
    }
    
    m_BVHData.subMeshBVHs.resize(m_MeshData.subMeshes.size());
    m_BVHData.subMeshAABBs.resize(m_MeshData.subMeshes.size());
    // 原始位置由 subMeshAABBs（TLAS 构建用）+ 实体变换承担；缓存键=规范化几何 hash。
    // 未来光追打开时设 s_buildBLAS=true（可加命令行/配置项）。
    static bool s_buildBLAS = false;
    static std::unordered_map<uint64_t, std::shared_ptr<ModelBVH>> s_blasSharedCache;
    for (size_t i = 0; i < m_MeshData.subMeshes.size(); i++) {
        const auto& sm = m_MeshData.subMeshes[i];
        std::filesystem::path pathObj(path);

        // 原始 subMesh AABB（含放置偏移，TLAS 用）
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
        for (const auto& v : sm.vertices) {
            mn.x = std::min(mn.x, v.Position.x); mn.y = std::min(mn.y, v.Position.y); mn.z = std::min(mn.z, v.Position.z);
            mx.x = std::max(mx.x, v.Position.x); mx.y = std::max(mx.y, v.Position.y); mx.z = std::max(mx.z, v.Position.z);
        }
        m_BVHData.subMeshAABBs[i] = AABB(mn, mx);

        if (!s_buildBLAS) {
            // 非光追：不构建三角形级 BVH（TLAS + subMeshAABBs 足够视锥剔除）
            m_BVHData.subMeshBVHs[i] = nullptr;
            continue;
        }

        const glm::vec3 center = m_BVHData.subMeshAABBs[i].GetCenter();

        // 规范化顶点（去中心）→ BLAS 构建 + 共享键
        uint64_t normHash = 0;
        std::vector<Vertex> normVerts;
        normVerts = sm.vertices;
        for (auto& v : normVerts) v.Position -= center;
        normHash = ComputeGeometryHash(normVerts.data(), normVerts.size(), sizeof(Vertex), sm.indices);
        std::string subMeshModelPath = (pathObj.parent_path() / (pathObj.stem().string() + "_submesh_" + std::to_string(normHash) + pathObj.extension().string())).string();

        // 内存共享：同规范化几何已在缓存 → 直接复用
        auto sharedIt = s_blasSharedCache.find(normHash);
        if (sharedIt != s_blasSharedCache.end()) {
            m_BVHData.subMeshBVHs[i] = sharedIt->second;
            continue;
        }
        auto blas = std::make_shared<ModelBVH>();
        if (!blas->loadFromFile(subMeshModelPath)) {
            blas->BuildFromSubMesh(normVerts, sm.indices);
            blas->saveToFile(subMeshModelPath);
        }
        s_blasSharedCache[normHash] = blas;
        m_BVHData.subMeshBVHs[i] = blas;
    }

    // 构建或加载顶层BVH（仅对多submesh模型）
    if (m_BVHData.subMeshBVHs.size() > 1) {
        // 尝试从磁盘加载TLAS
        if (!m_BVHData.loadTLASFromFile(path)) {
            // 其余成员 subMesh 从树中丢失 → BVH 视锥剔除漏剔（Bistro 1591→471 叶子，视锥内 46 个 subMesh 查不到）。
            // 全量叶子（1591）树深约 11 层，查询开销可接受；遮挡/射线若需简化 TLAS 应另存合并映射。

            // 构建新的TLAS
            m_BVHData.BuildTopLevelBVH();
            if (m_BVHData.HasTopLevelBVH()) {
                std::cout << "Top-level BVH built with " << m_BVHData.topLevelNodes.size() << " nodes" << std::endl;
                // 保存到磁盘
                m_BVHData.saveTLASToFile(path);
            }
        }
    }

    // [diag] 模型加载完成后的资源状态（交换链重建后蒙皮模型不可见排查；前几次打印对比启动 vs 重建）
    {
        static int s_setDiag = 0;
        if (s_setDiag < 8) {
            s_setDiag++;
            printf("[ModelRenderer][diag] loaddone path='%s' bones=%zu submeshes=%zu hasAnim=%d hasSkin=%d skinVB=%p skinMapped=%p ubo=%p boneView=%p\n",
                   m_ModelData.modelPath.c_str(), m_MeshData.bones.size(),
                   m_ModelData.subMeshes.size(),
                   m_ModelData.hasAnimation ? 1 : 0, m_ModelData.hasSkinning ? 1 : 0,
                   (void*)m_ModelData.skinnedVertexBuffer, (void*)m_ModelData.skinnedBufferMapped,
                   (void*)m_ModelData.uniformBuffer, (void*)m_ModelData.boneBufferView);
            for (size_t i = 0; i < m_ModelData.subMeshes.size() && i < 2; i++) {
                const auto& sm = m_ModelData.subMeshes[i];
                printf("[ModelRenderer][diag]   submesh[%zu] set=%p vb=%p ib=%p idx=%u\n",
                       i, (void*)sm.descriptorSet, (void*)sm.vertexBuffer,
                       (void*)sm.indexBuffer, sm.indexCount);
            }
            // [diag] obj 不渲染排查：顶点数/位置范围(NaN 检测)/材质纹理路径（静态/蒙皮布局位置均在首字段）
            for (size_t i = 0; i < m_MeshData.subMeshes.size() && i < 2; i++) {
                const auto& sm = m_MeshData.subMeshes[i];
                glm::vec3 mn(1e30f), mx(-1e30f);
                bool nan = false;
                for (size_t v = 0; v < sm.vertices.size() && v < 30000; v++) {
                    const glm::vec3& p = sm.vertices[v].Position;
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) { nan = true; break; }
                    mn = glm::min(mn, p); mx = glm::max(mx, p);
                }
                printf("[ModelRenderer][diag]   mesh[%zu] verts=%zu idx=%zu posRange=(%g,%g,%g)-(%g,%g,%g) nan=%d\n",
                       i, sm.vertices.size(), sm.indices.size(),
                       mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, nan ? 1 : 0);
            }
            for (size_t i = 0; i < m_MeshData.materialTextures.size() && i < 4; i++) {
                const auto& mt = m_MeshData.materialTextures[i];
                printf("[ModelRenderer][diag]   mtl[%zu] diff='%s' normal='%s' rough='%s' metal='%s' hasTex=%d hasNormal=%d hasRough=%d hasMetal=%d\n",
                       i, mt.diffuseTexturePath.c_str(), mt.normalTexturePath.c_str(),
                       mt.roughnessTexturePath.c_str(), mt.metallicTexturePath.c_str(),
                       mt.hasTexture ? 1 : 0, mt.hasNormalTexture ? 1 : 0,
                       mt.hasRoughnessTexture ? 1 : 0, mt.hasMetallicTexture ? 1 : 0);
            }
        }
    }
}

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

void ModelRenderer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
    if (g_CommandPool == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE) {
        return;
    }
    
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    if (vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        return;
    }
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
    
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    if (vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    if (vkQueueWaitIdle(g_Queue) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
}

void ModelRenderer::CreateUniformBuffer()
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    // 骨骼蒙皮矩阵 buffer（UBO 固定 64，vertex 动态索引；usage 含 UNIFORM_BUFFER 主路径 + TEXEL/STORAGE 兼容旧路径）
    bufferInfo.size = MAX_BONES * sizeof(glm::mat4) * ModelRenderData::MAX_FRAMES_IN_FLIGHT;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_ModelData.uniformBuffer) != VK_SUCCESS) {
        return;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_ModelData.uniformBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_ModelData.uniformBufferMemory) != VK_SUCCESS) {
        return;
    }
    
    vkBindBufferMemory(g_Device, m_ModelData.uniformBuffer, m_ModelData.uniformBufferMemory, 0);

    // 持久映射（骨骼矩阵每帧更新；3 帧槽全量映射，每帧写 frameIndex 偏移槽）
    vkMapMemory(g_Device, m_ModelData.uniformBufferMemory, 0,
        MAX_BONES * sizeof(glm::mat4) * ModelRenderData::MAX_FRAMES_IN_FLIGHT, 0, &m_ModelData.boneBufferMapped);

    // 骨骼矩阵 texel buffer 视图（uniform texel buffer，每骨骼 4 个 vec4 列）
    // 必须在 SetupDescriptorSets 之前创建（descriptor 写入时视图必须有效，否则 GPU 读空视图不渲染）
    if (m_ModelData.boneBufferView == VK_NULL_HANDLE) {
        VkBufferViewCreateInfo bvi{};
        bvi.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        bvi.buffer = m_ModelData.uniformBuffer;
        bvi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        bvi.offset = 0;
        bvi.range = MAX_BONES * sizeof(glm::mat4);
        vkCreateBufferView(g_Device, &bvi, g_Allocator, &m_ModelData.boneBufferView);
    }
}

void ModelRenderer::CreatePipeline(VkRenderPass renderPass)
{
    // 使用描述符集缓存中的布局
    VkDescriptorSetLayout descriptorLayout = DescriptorSetCache::GetInstance().GetLayout();

    PipelineConfig config;
    config.vertShader = "model.vert.spv";
    config.fragShader = "model.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.cullMode = VK_CULL_MODE_BACK_BIT;
    config.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    // Desktop subpass 1 also declares the reserved composite slot (index 4).
    // It must have a blend-state entry, but must never receive geometry output.
    config.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        0
    };
    config.subpass = 1;               // MRT 几何 subpass（0=z-prepass depth-only）
    // z-prepass 后 MRT 深度测试必须 LESS_OR_EQUAL——z-prepass 写的深度与本阶段片元深度几乎相等，LESS 严格小于会剔除内部像素只剩剪影
    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // z-prepass 关闭时（g_EnableZPrepass=false）几何 pass 恢复写深度（深度附件无预填）。
    // ⚠️ 运行时切换 g_EnableZPrepass 需重启引擎（管线创建时固化）；BLEND 半透明不受影响（不写深度语义一致）。
    config.depthWrite = !g_EnableZPrepass;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(ModelUniformData) + 32;
    
    VkVertexInputBindingDescription vertexBindingDesc = {};
    vertexBindingDesc.binding = 0;
    vertexBindingDesc.stride = sizeof(Vertex);
    vertexBindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    config.vertexBindings.push_back(vertexBindingDesc);
    
    VkVertexInputBindingDescription instanceBindingDesc = {};
    instanceBindingDesc.binding = 1;
    instanceBindingDesc.stride = sizeof(ModelInstanceData);
    instanceBindingDesc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    config.vertexBindings.push_back(instanceBindingDesc);
    
    std::vector<VkVertexInputAttributeDescription> attrDescs;
    
    VkVertexInputAttributeDescription posAttr = {};
    posAttr.binding = 0;
    posAttr.location = 0;
    posAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    posAttr.offset = offsetof(Vertex, Position);
    attrDescs.push_back(posAttr);
    
    VkVertexInputAttributeDescription normalAttr = {};
    normalAttr.binding = 0;
    normalAttr.location = 1;
    normalAttr.format = VK_FORMAT_R8G8B8A8_SNORM;   // 压缩：int8 驱动自动归一化 -1..1
    normalAttr.offset = offsetof(Vertex, Normal);
    attrDescs.push_back(normalAttr);
    
    VkVertexInputAttributeDescription texAttr = {};
    texAttr.binding = 0;
    texAttr.location = 2;
    texAttr.format = VK_FORMAT_R16G16_SFLOAT;   // 压缩：half float
    texAttr.offset = offsetof(Vertex, TexCoords);
    attrDescs.push_back(texAttr);
    
    VkVertexInputAttributeDescription tangentAttr = {};
    tangentAttr.binding = 0;
    tangentAttr.location = 3;
    tangentAttr.format = VK_FORMAT_R8G8B8A8_SNORM;   // 压缩：xyz 方向 + w 手性（Bitangent 不再存储，shader 推导）
    tangentAttr.offset = offsetof(Vertex, Tangent);
    attrDescs.push_back(tangentAttr);
    
    // location 4: Bitangent 已移除（压缩顶点不再存储；shader 由 cross(normal, tangent.xyz)*tangent.w 推导）
    
    // Instance attributes (location 5-8: model matrix)
    for (int i = 0; i < 4; i++) {
        VkVertexInputAttributeDescription modelAttr = {};
        modelAttr.binding = 1;
        modelAttr.location = 5 + i;
        modelAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        modelAttr.offset = offsetof(ModelInstanceData, model) + sizeof(glm::vec4) * i;
        attrDescs.push_back(modelAttr);
    }
    
    // location 9-12: prevModel matrix (mat4)
    for (int i = 0; i < 4; i++) {
        VkVertexInputAttributeDescription prevModelAttr = {};
        prevModelAttr.binding = 1;
        prevModelAttr.location = 9 + i;
        prevModelAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        prevModelAttr.offset = offsetof(ModelInstanceData, prevModel) + sizeof(glm::vec4) * i;
        attrDescs.push_back(prevModelAttr);
    }
    
    // location 13: albedoColor (vec4)
    VkVertexInputAttributeDescription albedoColorAttr = {};
    albedoColorAttr.binding = 1;
    albedoColorAttr.location = 13;
    albedoColorAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    albedoColorAttr.offset = offsetof(ModelInstanceData, albedoColor);
    attrDescs.push_back(albedoColorAttr);
    
    // location 14: materialData (vec4) - metallic, roughness, ao, useAlbedoTexture
    VkVertexInputAttributeDescription materialDataAttr = {};
    materialDataAttr.binding = 1;
    materialDataAttr.location = 14;
    materialDataAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    materialDataAttr.offset = offsetof(ModelInstanceData, materialData);
    attrDescs.push_back(materialDataAttr);
    
    // location 15: textureFlags (vec4) - useNormalTexture, padding...
    VkVertexInputAttributeDescription textureFlagsAttr = {};
    textureFlagsAttr.binding = 1;
    textureFlagsAttr.location = 15;
    textureFlagsAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    textureFlagsAttr.offset = offsetof(ModelInstanceData, textureFlags);
    attrDescs.push_back(textureFlagsAttr);
    
    // location 16: BoneIDs (u8vec4) - 骨骼蒙皮（无骨骼模型全 0xFF/weight=0，shader clamp 跳过）
    VkVertexInputAttributeDescription boneIdsAttr = {};
    boneIdsAttr.binding = 0;
    boneIdsAttr.location = 16;
    boneIdsAttr.format = VK_FORMAT_R8G8B8A8_UINT;   // 压缩：uint8 ×4
    boneIdsAttr.offset = offsetof(Vertex, BoneIDs);
    attrDescs.push_back(boneIdsAttr);

    // location 17: BoneWeights (vec4) - UNORM 驱动自动归一化 0..1
    VkVertexInputAttributeDescription boneWeightsAttr = {};
    boneWeightsAttr.binding = 0;
    boneWeightsAttr.location = 17;
    boneWeightsAttr.format = VK_FORMAT_R8G8B8A8_UNORM;   // 压缩：uint8 量化 0-255
    boneWeightsAttr.offset = offsetof(Vertex, BoneWeights);
    attrDescs.push_back(boneWeightsAttr);
    
    config.vertexAttributes = attrDescs;

    m_VertexBindings = config.vertexBindings;
    m_VertexAttributes = config.vertexAttributes;

    if (!m_ModelData.pipeline.Create(renderPass, descriptorLayout, config)) {
        return;
    }
    
    // 创建双面渲染管线（禁用背面剔除）
    PipelineConfig doubleSidedConfig = config;
    doubleSidedConfig.cullMode = VK_CULL_MODE_NONE;  // 双面渲染，不剔除任何面
    
    if (!m_ModelData.doubleSidedPipeline.Create(renderPass, descriptorLayout, doubleSidedConfig)) {
        return;
    }
    
    // 创建线框渲染管线
    PipelineConfig wireframeConfig = config;
    wireframeConfig.polygonMode = VK_POLYGON_MODE_LINE;  // 线框模式
    wireframeConfig.cullMode = VK_CULL_MODE_NONE;  // 线框模式下不剔除任何面
    
    if (!m_ModelData.wireframePipeline.Create(renderPass, descriptorLayout, wireframeConfig)) {
        return;
    }

    // ===== z-prepass depth-only 管线（subpass 0）：与主 model 管线同顶点布局/蒙皮，仅输出深度 =====
    // 复用 config（顶点绑定/属性/蒙皮 UBO/push constant 一致）；蒙皮 zprepass 绑完整 set（蒙皮 UBO binding 4）
    if (!g_UseSeparateMrtRenderPass) {
        PipelineConfig depthConfig = config;
        depthConfig.vertShader = "zprepass.vert.spv";
        depthConfig.fragShader = "model_zprepass.frag.spv";
        depthConfig.colorAttachmentCount = kMainMrtZPrepassColorAttachmentCount;
        // Desktop subpass 0 has the reserved composite color attachment even for
        // the depth-only pipeline; keep its blend slot disabled.
        depthConfig.colorWriteMasks = { 0 };
        depthConfig.subpass = 0;                   // z-prepass subpass
        depthConfig.depthCompareOp = VK_COMPARE_OP_LESS;
        depthConfig.depthWrite = true;
        if (!m_ModelData.depthPipeline.Create(renderPass, descriptorLayout, depthConfig)) {
            return;
        }
    }
}

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

void ModelRenderer::RefreshBoneMatricesAndSkinning() {
    auto& md = m_ModelData;
    if (!md.hasSkinning && !md.hasAnimation) return;

    // 蒙皮矩阵（global * offsetMatrix）写入 UBO buffer（保留，供调试/后续 GPU 蒙皮）
    if (md.boneBufferMapped) {
        g_BoneDynamicOffset = (uint32_t)(GetCurrentFrameIndex() % ModelRenderData::MAX_FRAMES_IN_FLIGHT)
                            * (uint32_t)(MAX_BONES * sizeof(glm::mat4));
        md.boneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
        for (size_t i = 0; i < m_MeshData.bones.size() && i < MAX_BONES; i++) {
            md.boneMatrices[i] = m_MeshData.bones[i].globalTransform * m_MeshData.bones[i].offsetMatrix;
        }
        memcpy((char*)md.boneBufferMapped + g_BoneDynamicOffset, md.boneMatrices.data(), MAX_BONES * sizeof(glm::mat4));
        // [diag-20260806] GPU 蒙皮数据验证：mapped buffer 内容 vs boneMatrices（应一致；数值应合理非飞点）
        {
            static int s_gpuSkinDiag = 0;
            if (s_gpuSkinDiag < 3) {
                s_gpuSkinDiag++;
                glm::mat4* mapped = (glm::mat4*)md.boneBufferMapped;
                printf("[diag] GPUskin: bm[0].c0=(%.3f,%.3f,%.3f,%.3f) mapped[0].c0=(%.3f,%.3f,%.3f,%.3f) mapped[0].c3=(%.3f,%.3f,%.3f,%.3f) bones=%zu\n",
                    md.boneMatrices[0][0][0], md.boneMatrices[0][0][1], md.boneMatrices[0][0][2], md.boneMatrices[0][0][3],
                    mapped[0][0][0], mapped[0][0][1], mapped[0][0][2], mapped[0][0][3],
                    mapped[0][3][0], mapped[0][3][1], mapped[0][3][2], mapped[0][3][3],
                    m_MeshData.bones.size());
            }
        }
    }

    // [diag] 蒙皮矩阵异常检测（交换链重建后蒙皮模型消失排查；仅打印前几次避免刷屏）
    {
        static int s_skinDiag = 0;
        bool anyBad = false;
        for (size_t i = 0; i < m_MeshData.bones.size() && i < MAX_BONES && !anyBad; i++) {
            const glm::mat4& bm = md.boneMatrices[i];
            for (int r = 0; r < 4 && !anyBad; r++) {
                for (int c = 0; c < 4 && !anyBad; c++) {
                    if (!std::isfinite(bm[r][c])) { anyBad = true; break; }
                }
            }
            if (anyBad && s_skinDiag < 5) {
                s_skinDiag++;
                printf("[ModelRenderer][diag] SKIN MATRIX BAD: path='%s' bone=%zu time=%.4f\n",
                       m_ModelData.modelPath.c_str(), i, md.animTime);
            }
        }
    }

    // ===== CPU 蒙皮（fallback：仅当 GPU 蒙皮关闭时执行；GPU 主路径由顶点着色器蒙皮）=====
    if (!g_UseGpuSkinning && md.hasSkinning && md.skinnedBufferMapped &&
        md.skinnedSubMeshes.size() == m_MeshData.subMeshes.size()) {
        for (size_t s = 0; s < m_MeshData.subMeshes.size(); s++) {
            const auto& src = m_MeshData.subMeshes[s].vertices;
            auto& dst = md.skinnedSubMeshes[s];
            dst.resize(src.size());
            for (size_t i = 0; i < src.size(); i++) {
                const Vertex& v = src[i];
                dst[i] = v;
                // 解包压缩权重/ID（UNORM uint8 → 0..1；UINT8 直读）
                const glm::vec4 w = glm::vec4(v.BoneWeights) * (1.0f / 255.0f);
                const glm::vec3 vNormal = UnpackSnorm3(v.Normal);
                const glm::vec3 vTangent = UnpackSnorm3(v.Tangent);
                const glm::vec3 vBitangent = glm::normalize(glm::cross(vNormal, vTangent)) * (v.Tangent.w >= 0 ? 1.0f : -1.0f);
                const float tw = w.x + w.y + w.z + w.w;
                if (tw > 0.001f) {
                    glm::mat4 sm(0.0f);
                    if (w.x > 0.0f) sm += w.x * md.boneMatrices[v.BoneIDs.x];
                    if (w.y > 0.0f) sm += w.y * md.boneMatrices[v.BoneIDs.y];
                    if (w.z > 0.0f) sm += w.z * md.boneMatrices[v.BoneIDs.z];
                    if (w.w > 0.0f) sm += w.w * md.boneMatrices[v.BoneIDs.w];
                    dst[i].Position = glm::vec3(sm * glm::vec4(v.Position, 1.0f));
                    // 蒙皮后重编码压缩（法线/切线 SNORM + 手性）
                    const glm::vec3 newN = glm::normalize(glm::mat3(sm) * vNormal);
                    const glm::vec3 newT = glm::normalize(glm::mat3(sm) * vTangent);
                    const glm::vec3 newB = glm::normalize(glm::mat3(sm) * vBitangent);
                    const float hand = glm::dot(glm::cross(newN, newT), newB) >= 0.0f ? 1.0f : -1.0f;
                    dst[i].Normal  = PackSnorm3(newN);
                    dst[i].Tangent = PackSnorm3(newT, hand);
                }
                // CPU 蒙皮已完成：清空骨骼权重/ID，防止顶点着色器再次蒙皮（双重蒙皮导致姿态错乱）
                dst[i].BoneWeights = glm::u8vec4(0, 0, 0, 0);
                dst[i].BoneIDs = glm::u8vec4(0xFF, 0xFF, 0xFF, 0xFF);
            }
            memcpy((char*)md.skinnedBufferMapped + md.skinnedBufferOffsets[s],
                   dst.data(), dst.size() * sizeof(Vertex));
        }
    }
}

// ===== 骨骼动画 =====
void ModelRenderer::UpdateAnimation(float deltaTime) {
    auto& md = m_ModelData;
    if (!md.hasSkinning && (!md.hasAnimation || !md.animPlaying || m_MeshData.animations.empty())) return;

    if (md.hasAnimation && md.animPlaying && !m_MeshData.animations.empty()) {
    const int clipCount = (int)m_MeshData.animations.size();
    if (md.currentClip < 0 || md.currentClip >= clipCount) md.currentClip = 0;
    const AnimationClip& clip = m_MeshData.animations[md.currentClip];

    md.animTime += deltaTime * md.animSpeed;
    if (clip.duration > 0.0f) {
        if (md.animLoop) {
            md.animTime = fmodf(md.animTime, clip.duration);
        } else if (md.animTime >= clip.duration) {
            md.animTime = clip.duration;
            md.animPlaying = false;
        }
    }

    // [diag] 动画时间异常检测：NaN/Inf 会让骨骼采样与蒙皮矩阵全坏（交换链重建后模型消失排查）
    if (!std::isfinite(md.animTime)) {
        static int s_timeDiag = 0;
        if (s_timeDiag < 5) {
            s_timeDiag++;
            printf("[ModelRenderer][diag] ANIM TIME BAD: path='%s' time=%f clip=%d loop=%d playing=%d\n",
                   m_ModelData.modelPath.c_str(), md.animTime, md.currentClip, md.animLoop ? 1 : 0, md.animPlaying ? 1 : 0);
        }
        md.animTime = 0.0f;
        md.animPlaying = true;
    }

    // 采样动画 -> 更新骨骼局部/全局变换
    ModelLoader::SampleAnimation(clip, md.animTime, m_MeshData.bones);
    }

    RefreshBoneMatricesAndSkinning();
}

bool ModelRenderer::ApplyBoneLocalPose(const std::vector<glm::mat4>& localTransforms) {
    if (!m_ModelData.hasSkinning || localTransforms.size() != m_MeshData.bones.size()) {
        return false;
    }

    for (size_t i = 0; i < m_MeshData.bones.size(); ++i) {
        m_MeshData.bones[i].localTransform = localTransforms[i];
    }

    // 重新计算 globalTransform。骨骼父节点必须先算，根骨骼保留加载阶段的
    // ancestorTransform（例如模型的 Z_UP/Armature 轴修正）。
    std::vector<bool> computed(m_MeshData.bones.size(), false);
    for (size_t pass = 0; pass < m_MeshData.bones.size(); ++pass) {
        bool any = false;
        for (size_t i = 0; i < m_MeshData.bones.size(); ++i) {
            if (computed[i]) continue;
            Bone& bone = m_MeshData.bones[i];
            const int parent = bone.parentIndex;
            const bool validParent = parent >= 0 && parent < (int)m_MeshData.bones.size();
            if (!validParent || computed[(size_t)parent]) {
                bone.globalTransform = validParent
                    ? m_MeshData.bones[(size_t)parent].globalTransform * bone.localTransform
                    : bone.ancestorTransform * bone.localTransform;
                bone.position = glm::vec3(bone.globalTransform[3]);
                bone.rotation = glm::quat_cast(bone.globalTransform);
                computed[i] = true;
                any = true;
            }
        }
        if (!any) break;
    }

    // 若资源存在损坏的父索引环，仍为剩余骨骼保留一个确定姿态，避免把旧帧
    // 的 globalTransform 带入本帧蒙皮。
    for (size_t i = 0; i < m_MeshData.bones.size(); ++i) {
        if (computed[i]) continue;
        Bone& bone = m_MeshData.bones[i];
        bone.globalTransform = bone.ancestorTransform * bone.localTransform;
        bone.position = glm::vec3(bone.globalTransform[3]);
        bone.rotation = glm::quat_cast(bone.globalTransform);
    }

    RefreshBoneMatricesAndSkinning();
    return true;
}

void ModelRenderer::PlayAnimation(int clipIndex, bool loop) {
    if (m_MeshData.animations.empty()) return;
    m_ModelData.currentClip = glm::clamp(clipIndex, 0, (int)m_MeshData.animations.size() - 1);
    m_ModelData.animLoop = loop;
    m_ModelData.animTime = 0.0f;
    m_ModelData.animPlaying = true;
}

int ModelRenderer::GetAnimationCount() const {
    return (int)m_MeshData.animations.size();
}

const char* ModelRenderer::GetAnimationName() const {
    if (m_MeshData.animations.empty()) return "";
    const int idx = m_ModelData.currentClip;
    if (idx < 0 || idx >= (int)m_MeshData.animations.size()) return "";
    return m_MeshData.animations[idx].name.c_str();
}

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

void ModelRenderer::CreateInstanceBuffer(size_t maxInstances)
{
    auto [stateIt, inserted] = g_InstanceUploadStates.try_emplace(this);
    (void)inserted;
    DestroyInstanceUploadState(stateIt->second);

    for (size_t frame = 0; frame < ModelRenderData::MAX_FRAMES_IN_FLIGHT; ++frame) {
        m_ModelData.instanceBuffers[frame] = VK_NULL_HANDLE;
        m_ModelData.instanceBufferMemories[frame] = VK_NULL_HANDLE;
        m_ModelData.instanceBufferMapped[frame] = nullptr;
    }
    m_ModelData.instanceBufferSize = maxInstances;
    m_ModelData.currentInstanceBufferSize = maxInstances;

    // 槽按需创建；这里预留第一个槽只为了让 Init 后的句柄行为与旧实现
    // 一致，但不把 Vulkan 资源写入 ModelRenderData 的可变容器。
    auto& firstBucket = stateIt->second.slots[0];
    firstBucket.emplace_back();
    if (!CreateInstanceUploadSlot(maxInstances, firstBucket.back())) {
        firstBucket.clear();
        return;
    }
    m_ModelData.instanceBuffer = firstBucket.back().buffer;
    m_ModelData.instanceBufferMemory = firstBucket.back().memory;
}

void ModelRenderer::UpdateInstanceBuffer(const std::vector<ModelInstanceData>& instanceData)
{
    if (instanceData.empty()) {
        return;
    }

    auto stateIt = g_InstanceUploadStates.find(this);
    if (stateIt == g_InstanceUploadStates.end()) {
        CreateInstanceBuffer(std::max<size_t>(instanceData.size() * 2, 4096));
        stateIt = g_InstanceUploadStates.find(this);
        if (stateIt == g_InstanceUploadStates.end()) return;
    }
    auto& state = stateIt->second;
    const uint32_t frameIndex = GetCurrentFrameIndex() % ModelRenderData::MAX_FRAMES_IN_FLIGHT;
    const uint64_t frameSerial = GetCurrentFrameSerial();
    if (state.frameSerial != frameSerial) {
        // FrameRender 已等待当前 swapchain image 的 fence；这个 bucket 的
        // 所有旧槽现在可复用，但本帧内 cursor 不能回退。
        state.frameSerial = frameSerial;
        state.cursor[frameIndex] = 0;
    }

    auto& uploads = state.slots[frameIndex];
    const size_t uploadIndex = state.cursor[frameIndex]++;
    if (uploads.size() <= uploadIndex) {
        uploads.resize(uploadIndex + 1);
    }
    auto& upload = uploads[uploadIndex];
    const size_t requiredCapacity = instanceData.size();

    if (upload.buffer == VK_NULL_HANDLE || upload.mapped == nullptr || upload.capacity < requiredCapacity) {
        DestroyInstanceUploadSlot(upload);
        const size_t newCapacity = std::max(requiredCapacity * 2,
            std::max<size_t>(m_ModelData.currentInstanceBufferSize, 4096));
        if (!CreateInstanceUploadSlot(newCapacity, upload)) {
            return;
        }
        m_ModelData.currentInstanceBufferSize = std::max(m_ModelData.currentInstanceBufferSize, newCapacity);
        m_ModelData.instanceBufferSize = m_ModelData.currentInstanceBufferSize;
    }

    memcpy(upload.mapped, instanceData.data(), sizeof(ModelInstanceData) * instanceData.size());
    m_ModelData.instanceBuffer = upload.buffer;
    m_ModelData.instanceBufferMemory = upload.memory;
}

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

void ModelRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
    (void)commandBuffer;
    (void)view;
    (void)proj;
}

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

void ModelRenderer::EnsureShadowPipelines(VkRenderPass shadowRenderPass)
{
    if (m_ModelData.shadowDepthPipeline.GetPipeline() != VK_NULL_HANDLE ) {
        return;   // 已创建
    }
    PipelineConfig config;
    config.vertShader = "shadow_depth.vert.spv";
    config.fragShader = "shadow_depth.frag.spv";
    config.cullMode = VK_CULL_MODE_NONE;
    config.colorAttachmentCount = 0;   // depth-only
    config.subpass = 0;
    config.depthCompareOp = VK_COMPARE_OP_LESS;
    config.depthWrite = true;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(glm::mat4) + sizeof(glm::vec4) * 2;   // projView + lightPosRange + subMeshAlpha = 96B
    config.vertexBindings = m_VertexBindings;
    config.vertexAttributes = m_VertexAttributes;
    if (!m_ModelData.shadowDepthPipeline.Create(shadowRenderPass, DescriptorSetCache::GetInstance().GetLayout(), config)) {
        return;
    }
}

void ModelRenderer::RenderShadowDepth(VkCommandBuffer commandBuffer, int width, int height,
                                      const glm::mat4& projView, const glm::vec3& lightPos, float range,
                                      const std::vector<ModelInstanceData>& instanceData,
                                      const std::vector<size_t>& visibleSubMeshIndices)
{
    if (m_ModelData.shadowDepthPipeline.GetPipeline() == VK_NULL_HANDLE ||
        instanceData.empty() || m_ModelData.subMeshes.empty()) {
        return;
    }
    struct ShadowPush {
        glm::mat4 projView;
        glm::vec4 lightPosRange;
        glm::vec4 subMeshAlpha;
    };   // 96B，与 shader push 一致
    ShadowPush push = {};
    push.projView = projView;
    push.lightPosRange = glm::vec4(lightPos, range);
    UpdateInstanceBuffer(instanceData);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    for (size_t sortedIdx : m_ModelData.cachedSortedIndices) {
        if (!visibleSubMeshIndices.empty() &&
            std::find(visibleSubMeshIndices.begin(), visibleSubMeshIndices.end(), sortedIdx) == visibleSubMeshIndices.end()) {
            continue;
        }
        const auto& subMesh = m_ModelData.subMeshes[sortedIdx];
        if (subMesh.alphaMode == 2) {
            // BLEND 没有二值遮罩，不应作为实体写入点光源阴影图。
            continue;
        }
        const VkPipeline pipe = m_ModelData.shadowDepthPipeline.GetPipeline();
        const VkPipelineLayout layout = m_ModelData.shadowDepthPipeline.GetLayout();
        if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) continue;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        push.subMeshAlpha = glm::vec4(subMesh.alphaCutoff, (float)subMesh.alphaMode, 0.0f, 0.0f);
        vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowPush), &push);

            const VkDescriptorSet curSet = subMesh.descriptorSet;   // 蒙皮阴影：bone UBO binding 4
            if (curSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &curSet, 1, &g_BoneDynamicOffset);
            }
        
        VkBuffer vertexBuffers[2] = {VK_NULL_HANDLE, m_ModelData.instanceBuffer};
        vertexBuffers[0] = subMesh.vertexBuffer;
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
    }
}

void ModelRenderer::EnsureCsmPipelines(VkRenderPass csmRenderPass)
{
    if (m_ModelData.csmDepthPipeline.GetPipeline() != VK_NULL_HANDLE ) {
        return;   // 已创建
    }
    PipelineConfig config;
    config.vertShader = "shadow_depth.vert.spv";   // 蒙皮版顶点（输出 vWorldPos 未被 csm frag 使用，无害）
    config.fragShader = "csm_depth.frag.spv";
    config.colorAttachmentCount = 0;   // depth-only
    config.subpass = 0;
    config.depthCompareOp = VK_COMPARE_OP_LESS;
    config.depthWrite = true;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(glm::mat4) + sizeof(glm::vec4) * 2;   // projView + lightPosRange + subMeshAlpha = 96B
    // framebuffer 空间绕序反转 → cull FRONT 实际剔除 GL 背面、渲染 GL 正面（lit≈curD→自阴影）。
    // 改 cull NONE（渲染双面——与点光源阴影 2131 同款已验证）：单面也画、双面深度测试自动取最近面（正面）；
    // 自阴影由 depthBias（2.0/2.0）解决
    config.cullMode = VK_CULL_MODE_NONE;
    config.depthBiasEnable = true;
    config.depthBiasConstantFactor = 2.0f;
    config.depthBiasClamp = 0.0f;
    config.depthBiasSlopeFactor = 2.0f;
    config.vertexBindings = m_VertexBindings;
    config.vertexAttributes = m_VertexAttributes;
    if (!m_ModelData.csmDepthPipeline.Create(csmRenderPass, DescriptorSetCache::GetInstance().GetLayout(), config)) {
        return;
    }
}

void ModelRenderer::RenderCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                                   const glm::mat4& projView,
                                   const std::vector<ModelInstanceData>& instanceData,
                                   const std::vector<size_t>& visibleSubMeshIndices)
{
    if (m_ModelData.csmDepthPipeline.GetPipeline() == VK_NULL_HANDLE ||
        instanceData.empty() || m_ModelData.subMeshes.empty()) {
        return;
    }
    struct CsmPush {
        glm::mat4 projView;
        glm::vec4 unused;
        glm::vec4 subMeshAlpha;
    };   // 96B，与 shader push 一致
    CsmPush push = {};
    push.projView = projView;
    static bool s_pushLogged = false;
    if (!s_pushLogged) {
        s_pushLogged = true;
        LOGI("[CSM-MAT] push c?: m00=%.4f m11=%.4f m22=%.4f m33=%.4f row3=(%.4f,%.4f,%.4f,%.4f)",
             projView[0][0], projView[1][1], projView[2][2], projView[3][3],
             projView[3][0], projView[3][1], projView[3][2], projView[3][3]);
    }
    UpdateInstanceBuffer(instanceData);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    std::vector<uint8_t> visBitmap;
    if (!visibleSubMeshIndices.empty()) {
        visBitmap.assign(m_ModelData.subMeshes.size(), 0);
        for (size_t idx : visibleSubMeshIndices) {
            if (idx < visBitmap.size()) visBitmap[idx] = 1;
        }
    }

    for (size_t sortedIdx : m_ModelData.cachedSortedIndices) {
        if (!visBitmap.empty() && !visBitmap[sortedIdx]) continue;
        const auto& subMesh = m_ModelData.subMeshes[sortedIdx];
        if (subMesh.alphaMode == 2) {
            // BLEND 没有二值遮罩，不应作为实体写入 CSM 阴影图。
            continue;
        }
        const VkPipeline pipe = m_ModelData.csmDepthPipeline.GetPipeline();
        const VkPipelineLayout layout = m_ModelData.csmDepthPipeline.GetLayout();
        if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) continue;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        push.subMeshAlpha = glm::vec4(subMesh.alphaCutoff, (float)subMesh.alphaMode, 0.0f, 0.0f);
        vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CsmPush), &push);
            const VkDescriptorSet curSet = subMesh.descriptorSet;   // 蒙皮阴影：bone UBO binding 4
            if (curSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &curSet, 1, &g_BoneDynamicOffset);
            }
        
        VkBuffer vertexBuffers[2] = {VK_NULL_HANDLE, m_ModelData.instanceBuffer};
        vertexBuffers[0] = subMesh.vertexBuffer;
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, subMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commandBuffer, subMesh.indexCount, static_cast<uint32_t>(instanceData.size()), 0, 0, 0);
    }
}

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
    }    return false;
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
