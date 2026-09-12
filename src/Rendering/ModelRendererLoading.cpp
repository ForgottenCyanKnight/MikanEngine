#include "ModelRenderer.h"
#include "Core/Log.h"
#include "Core/VulkanContext.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

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

