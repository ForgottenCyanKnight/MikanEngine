#include "TerrainRenderer.h"

#include "Core/EngineConfig.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "Core/InputGlobals.h"
#include "ECS/SceneECS.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/RenderWorld.h"
#include "Rendering/HeightmapLoader.h"
#include "TexturePool.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <SDL3/SDL.h>
#include <SDL3/SDL_timer.h>
#include "Rendering/RenderStats.h"
#include "Core/Log.h"

#include "TerrainRendererInternal.h"

using namespace TerrainRendererInternal;

namespace {

float SmoothstepRange(float edge0, float edge1, float value) {
    const float denominator = edge1 - edge0;
    if (std::abs(denominator) <= 1e-8f) {
        return value < edge0 ? 0.0f : 1.0f;
    }
    const float t = std::clamp((value - edge0) / denominator, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 程序化控制图的初始权重：逐 texel 复刻 terrain.frag 在"没有控制图"分支里的坡度规则
// （缓坡=草、中坡=土、陡坡=岩）。因为写进去的是 pow(blendSharpness) 之前的原始权重，
// 着色器拿到后会和自动模式做完全一样的 pow + 归一化，所以从自动混合切到控制图
// 不会让已有场景的外观跳变 —— 这正是材质笔刷可以直接在"自有控制图"上开工的前提。
//
// 通道序与着色器一致：R=重心 weights.x → 图层0(草)，G=weights.y → 图层1(岩)，
// B=weights.z → 图层2(土)，A=weights.w → 图层3（自动模式恒为 0，留给笔刷）。
void ComputeSlopeBlendWeights(const uint16_t* samples, uint32_t width, uint32_t height,
                              const glm::vec2& worldSize, float heightScale,
                              std::vector<uint8_t>& outRgba) {
    const size_t texelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    outRgba.assign(texelCount * 4, 0);
    if (texelCount == 0) {
        return;
    }
    if (samples == nullptr || width < 2 || height < 2) {
        // 退化输入（理论上不会发生）：全部按草地填满，至少不会变成全黑地形。
        for (size_t texel = 0; texel < texelCount; ++texel) {
            outRgba[texel * 4 + 0] = 255;
        }
        return;
    }

    const float maxTexelX = static_cast<float>(width - 1);
    const float maxTexelY = static_cast<float>(height - 1);
    const float texelWorldX = std::max(std::abs(worldSize.x) / maxTexelX, 1e-4f);
    const float texelWorldZ = std::max(std::abs(worldSize.y) / maxTexelY, 1e-4f);
    const float inverseTwoTexelX = 1.0f / (2.0f * texelWorldX);
    const float inverseTwoTexelZ = 1.0f / (2.0f * texelWorldZ);
    const float normalizedToWorld = heightScale / 65535.0f;

    const int lastX = static_cast<int>(width) - 1;
    const int lastY = static_cast<int>(height) - 1;
    auto sampleAt = [&](int x, int y) {
        const int clampedX = std::clamp(x, 0, lastX);
        const int clampedY = std::clamp(y, 0, lastY);
        return static_cast<float>(samples[static_cast<size_t>(clampedY) * width +
                                           static_cast<size_t>(clampedX)]) * normalizedToWorld;
    };

    for (uint32_t y = 0; y < height; ++y) {
        const int iy = static_cast<int>(y);
        for (uint32_t x = 0; x < width; ++x) {
            const int ix = static_cast<int>(x);
            const float dHdX = (sampleAt(ix + 1, iy) - sampleAt(ix - 1, iy)) * inverseTwoTexelX;
            const float dHdZ = (sampleAt(ix, iy + 1) - sampleAt(ix, iy - 1)) * inverseTwoTexelZ;
            // 着色器只用到世界法线的 y 分量来算坡度，而地形实体的 model 在建立资源时
            // 还是单位矩阵（旋转/缩放要到 Prepare 才进来），所以这里直接用局部法线。
            const float normalY = 1.0f / std::sqrt(dHdX * dHdX + 1.0f + dHdZ * dHdZ);
            const float slope = std::clamp(1.0f - normalY, 0.0f, 1.0f);

            const float grassToDirt = SmoothstepRange(0.08f, 0.22f, slope);
            const float dirtToRock = SmoothstepRange(0.20f, 0.48f, slope);
            const float grass = 1.0f - grassToDirt;
            const float dirt = grassToDirt * (1.0f - dirtToRock);
            const float rock = dirtToRock;

            uint8_t* texel = &outRgba[(static_cast<size_t>(y) * width + x) * 4];
            texel[0] = static_cast<uint8_t>(std::lround(std::clamp(grass, 0.0f, 1.0f) * 255.0f));
            texel[1] = static_cast<uint8_t>(std::lround(std::clamp(rock, 0.0f, 1.0f) * 255.0f));
            texel[2] = static_cast<uint8_t>(std::lround(std::clamp(dirt, 0.0f, 1.0f) * 255.0f));
            texel[3] = 0;
        }
    }
}

ECS::TerrainComponent MakeTerrainSettings(const RenderTerrainData& source) {
    ECS::TerrainComponent target;
    target.enabled = source.enabled;
    target.heightmapPath = source.heightmapPath;
    target.worldSize = source.worldSize;
    target.heightScale = source.heightScale;
    target.heightOffset = source.heightOffset;
    target.chunkCount = source.chunkCount;
    target.patchResolution = source.patchResolution;
    target.viewDistance = source.viewDistance;
    target.lod0Distance = source.lod0Distance;
    target.lod1Distance = source.lod1Distance;
    target.maxLod = source.maxLod;
    target.wireframe = source.wireframe;
    target.materialTiling = source.materialTiling;
    target.blendSharpness = source.blendSharpness;
    target.layer0Path = source.layer0Path;
    target.layer1Path = source.layer1Path;
    target.layer2Path = source.layer2Path;
    target.layer3Path = source.layer3Path;
    target.controlMapPath = source.controlMapPath;
    target.sculptedHeightmapPath = source.sculptedHeightmapPath;
    target.paintedControlMapPath = source.paintedControlMapPath;
    target.paintedGrassPath = source.paintedGrassPath;
    target.paintedWaterPath = source.paintedWaterPath;
    return target;
}

VkVertexInputBindingDescription MakeVertexBinding(uint32_t binding, uint32_t stride, VkVertexInputRate inputRate) {
    VkVertexInputBindingDescription desc{};
    desc.binding = binding;
    desc.stride = stride;
    desc.inputRate = inputRate;
    return desc;
}

VkVertexInputAttributeDescription MakeVertexAttribute(uint32_t location, uint32_t binding,
                                                       VkFormat format, uint32_t offset) {
    VkVertexInputAttributeDescription desc{};
    desc.location = location;
    desc.binding = binding;
    desc.format = format;
    desc.offset = offset;
    return desc;
}

uint32_t MortonPart1By1(uint32_t value) {
    value &= 0x0000ffffu;
    value = (value | (value << 8u)) & 0x00ff00ffu;
    value = (value | (value << 4u)) & 0x0f0f0f0fu;
    value = (value | (value << 2u)) & 0x33333333u;
    value = (value | (value << 1u)) & 0x55555555u;
    return value;
}

uint32_t MortonCode2D(uint32_t x, uint32_t z) {
    return MortonPart1By1(x) | (MortonPart1By1(z) << 1u);
}

} // namespace

void TerrainChunkManager::Configure(const glm::vec2& worldSize, int chunkCount,
                                    float minHeight, float maxHeight,
                                    float viewDistance, float lod0Distance, float lod1Distance,
                                    int maxLod) {
    m_WorldSize = glm::max(worldSize, glm::vec2(1.0f));
    m_ChunkCount = std::clamp(chunkCount, 1, 256);
    m_ViewDistance = std::max(0.0f, viewDistance);
    m_Lod0Distance = std::max(0.0f, lod0Distance);
    m_Lod1Distance = std::max(m_Lod0Distance, lod1Distance);
    m_MaxLod = std::clamp(maxLod, 0, 2);

    m_Chunks.clear();
    m_Chunks.reserve(static_cast<size_t>(m_ChunkCount) * static_cast<size_t>(m_ChunkCount));
    for (auto& cache : m_VisibilityCaches) {
        cache.valid = false;
        for (auto& visible : cache.visible) {
            visible.clear();
        }
    }
    m_ActiveVisibilitySlot = 0;

    const glm::vec2 terrainOrigin = -m_WorldSize * 0.5f;
    const float chunkCountFloat = static_cast<float>(m_ChunkCount);
    const float lowHeight = std::min(minHeight, maxHeight) - 1.0f;
    const float highHeight = std::max(minHeight, maxHeight) + 1.0f;

    for (int z = 0; z < m_ChunkCount; ++z) {
        for (int x = 0; x < m_ChunkCount; ++x) {
            TerrainChunk chunk;
            chunk.x = x;
            chunk.z = z;
            const glm::vec2 normalizedMin(static_cast<float>(x) / chunkCountFloat,
                                          static_cast<float>(z) / chunkCountFloat);
            const glm::vec2 normalizedMax(static_cast<float>(x + 1) / chunkCountFloat,
                                          static_cast<float>(z + 1) / chunkCountFloat);
            // 用和 shader 相同的全局分区公式计算 CPU 包围盒，避免边界
            // 因为先除后乘/先乘后除的舍入差异而出现错误剔除。
            chunk.origin = terrainOrigin + normalizedMin * m_WorldSize;
            chunk.size = (normalizedMax - normalizedMin) * m_WorldSize;
            chunk.uvOrigin = glm::vec2(static_cast<float>(x) / static_cast<float>(m_ChunkCount),
                                       static_cast<float>(z) / static_cast<float>(m_ChunkCount));
            chunk.uvSize = glm::vec2(1.0f / static_cast<float>(m_ChunkCount));
            chunk.localBounds = AABB(
                glm::vec3(chunk.origin.x, lowHeight, chunk.origin.y),
                glm::vec3(chunk.origin.x + chunk.size.x, highHeight, chunk.origin.y + chunk.size.y));
            m_Chunks.push_back(chunk);
        }
    }

    // 以 Morton/Z-order 保存 Chunk，令相邻空间块在 CPU 遍历、可见列表
    // 构建和后续流式加载中保持更好的局部性。它不改变 LOD 判定规则。
    std::sort(m_Chunks.begin(), m_Chunks.end(), [](const TerrainChunk& lhs,
                                                   const TerrainChunk& rhs) {
        const uint32_t lhsCode = MortonCode2D(static_cast<uint32_t>(lhs.x),
                                              static_cast<uint32_t>(lhs.z));
        const uint32_t rhsCode = MortonCode2D(static_cast<uint32_t>(rhs.x),
                                              static_cast<uint32_t>(rhs.z));
        return lhsCode < rhsCode;
    });
}

void TerrainChunkManager::UpdateVisibility(const glm::vec3& cameraPosition,
                                           const glm::mat4& modelMatrix,
                                           const std::array<Plane, 6>& frustumPlanes,
                                           bool useFrustumCulling,
                                           int viewSlot) {
    // 视图槽位直接映射到独立缓存段：场景视图 0 / 游戏视图 1 / 反射探针 2。
    // 这里曾把槽位夹到 0..1，第三个视图会被夹进游戏视图的段并覆盖它的可见集
    // （探针是 1:1 纵横比的独立相机，可见 chunk 集合与主视图完全不同）。
    const int cacheSlot =
        std::clamp(viewSlot, 0, static_cast<int>(m_VisibilityCaches.size()) - 1);
    m_ActiveVisibilitySlot = cacheSlot;
    VisibilityCache& cache = m_VisibilityCaches[static_cast<size_t>(cacheSlot)];
    const bool cacheHit = cache.valid &&
                          cache.useFrustumCulling == useFrustumCulling &&
                          SameVec3(cache.cameraPosition, cameraPosition) &&
                          SameMat4(cache.modelMatrix, modelMatrix) &&
                          (!useFrustumCulling ||
                           SameFrustum(cache.frustumPlanes, frustumPlanes));
    if (cacheHit) {
        return;
    }

    for (auto& visible : cache.visible) {
        visible.clear();
        visible.reserve(m_Chunks.size() / 2 + 1);
    }

    struct SelectedChunk {
        const TerrainChunk* chunk = nullptr;
        int lod = 0;
    };
    std::vector<SelectedChunk> selected;
    selected.reserve(m_Chunks.size());
    std::vector<int8_t> lodGrid(static_cast<size_t>(m_ChunkCount) * m_ChunkCount, -1);

    // 第一遍只做可见性和 LOD 判定，保留规则网格坐标供第二遍查询邻居。
    for (const TerrainChunk& chunk : m_Chunks) {
        const AABB worldBounds = chunk.localBounds.Transform(modelMatrix);
        if (useFrustumCulling && !worldBounds.IsInsideFrustum(frustumPlanes)) {
            continue;
        }

        const float distance = glm::distance(cameraPosition, worldBounds.GetCenter());
        if (m_ViewDistance > 0.0f && distance > m_ViewDistance + worldBounds.GetMaxDimension() * 0.5f) {
            continue;
        }

        int lod = 0;
        if (m_MaxLod >= 1 && distance >= m_Lod0Distance) {
            lod = 1;
        }
        if (m_MaxLod >= 2 && distance >= m_Lod1Distance) {
            lod = 2;
        }
        lod = std::clamp(lod, 0, m_MaxLod);

        lodGrid[static_cast<size_t>(chunk.z) * m_ChunkCount + chunk.x] =
            static_cast<int8_t>(lod);
        selected.push_back({&chunk, lod});
    }

    auto coarserDelta = [&](int x, int z, int lod) -> uint32_t {
        if (x < 0 || z < 0 || x >= m_ChunkCount || z >= m_ChunkCount) {
            return 0;
        }
        const int neighborLod = lodGrid[static_cast<size_t>(z) * m_ChunkCount + x];
        if (neighborLod < 0) {
            return 0;
        }
        return static_cast<uint32_t>(std::clamp(neighborLod - lod, 0, 3));
    };

    // 每条边用 2 bit 保存“邻居比本块粗多少级”：-Z、+X、+Z、-X。
    // 顶点着色器据此折叠细网格边缘顶点，替代会产生地下黑墙的 skirt。
    for (const SelectedChunk& selection : selected) {
        const TerrainChunk& chunk = *selection.chunk;
        const int lod = selection.lod;
        const uint32_t edgeLodDeltas =
            coarserDelta(chunk.x, chunk.z - 1, lod) |
            (coarserDelta(chunk.x + 1, chunk.z, lod) << 2u) |
            (coarserDelta(chunk.x, chunk.z + 1, lod) << 4u) |
            (coarserDelta(chunk.x - 1, chunk.z, lod) << 6u);
        TerrainChunkInstance instance;
        instance.originSize = glm::vec4(chunk.origin.x, chunk.origin.y, chunk.size.x, chunk.size.y);
        // 将整数网格坐标传给 GPU，由 shader 统一计算全局坐标。
        // 相邻块的边界分别变成 (x + 1) / count 与 (x + 1) / count，
        // 不再依赖两条不同的浮点加法路径。
        instance.uvRect = glm::vec4(static_cast<float>(chunk.x),
                                    static_cast<float>(chunk.z),
                                    static_cast<float>(m_ChunkCount), 0.0f);
        instance.params.x = static_cast<float>(lod);
        instance.params.y = static_cast<float>(edgeLodDeltas);
        cache.visible[static_cast<size_t>(lod)].push_back(instance);
    }

    cache.useFrustumCulling = useFrustumCulling;
    cache.cameraPosition = cameraPosition;
    cache.modelMatrix = modelMatrix;
    cache.frustumPlanes = frustumPlanes;
    cache.valid = true;
}

const std::vector<TerrainChunkInstance>& TerrainChunkManager::GetVisible(int lod) const {
    static const std::vector<TerrainChunkInstance> empty;
    if (lod < 0 || lod >= static_cast<int>(
            m_VisibilityCaches[static_cast<size_t>(m_ActiveVisibilitySlot)].visible.size())) {
        return empty;
    }
    return m_VisibilityCaches[static_cast<size_t>(m_ActiveVisibilitySlot)]
        .visible[static_cast<size_t>(lod)];
}

size_t TerrainChunkManager::GetVisibleCount() const {
    size_t count = 0;
    for (const auto& visible :
         m_VisibilityCaches[static_cast<size_t>(m_ActiveVisibilitySlot)].visible) {
        count += visible.size();
    }
    return count;
}

TerrainRenderer::~TerrainRenderer() {
    Cleanup();
}

void TerrainRenderer::Init(VkRenderPass renderPass) {
    m_RenderPass = renderPass;
    m_TerrainHiZPreviousGameViewProj = glm::mat4(1.0f);
    m_TerrainHiZHasPreviousGameView = false;
    m_GrassHiZPreviousViewProj.fill(glm::mat4(1.0f));
    m_GrassHiZHasPreviousView.fill(false);

    // render pass 在交换链重建后会变化，管线需要跟随重建；地形资源和 descriptor set 可以复用。
    m_Pipeline.Cleanup();
    m_WireframePipeline.Cleanup();
    m_DepthPipeline.Cleanup();
    m_CsmDepthPipeline.Cleanup();
    m_CsmRenderPass = VK_NULL_HANDLE;
    m_GrassDepthPipeline.Cleanup();
    m_GrassCsmRenderPass = VK_NULL_HANDLE;

    if (!CreateDescriptorResources() || !CreatePipelines()) {
        LOGE("[TerrainRenderer] initialization failed");
        return;
    }
    LOGI("[TerrainRenderer] initialized");
}

void TerrainRenderer::Cleanup() {
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }

    for (auto& [entity, resource] : m_Resources) {
        if (resource) {
            DestroyResource(*resource);
        }
    }
    m_Resources.clear();
    m_PreparedResources.clear();
    m_VisibleChunkCount = 0;

    m_Pipeline.Cleanup();
    m_GrassPipeline.Cleanup();
    m_GrassDepthPipeline.Cleanup();
    m_GrassCsmRenderPass = VK_NULL_HANDLE;
    m_WireframePipeline.Cleanup();
    m_DepthPipeline.Cleanup();
    m_CsmDepthPipeline.Cleanup();
    m_WaterTargetPipeline.Cleanup();
    m_WaterTargetRenderPass = VK_NULL_HANDLE;
    m_CsmRenderPass = VK_NULL_HANDLE;
    CleanupGrassCullResources();
    CleanupTerrainMdiCullResources();
    DestroyDescriptorResources();

    if (m_OwnedWhiteFallback && g_TexturePool) {
        g_TexturePool->Release("white");
    }
    m_OwnedWhiteFallback = false;
    m_RenderPass = VK_NULL_HANDLE;
    m_TerrainHiZPreviousGameViewProj = glm::mat4(1.0f);
    m_TerrainHiZHasPreviousGameView = false;
    m_GrassHiZPreviousViewProj.fill(glm::mat4(1.0f));
    m_GrassHiZHasPreviousView.fill(false);
}

void TerrainRenderer::Prepare(const std::vector<ECS::Entity>& rootEntities,
                              const glm::vec3& cameraPosition,
                              const std::array<Plane, 6>& frustumPlanes,
                              bool useFrustumCulling,
                              int viewSlot) {
    m_PreparedResources.clear();
    m_VisibleChunkCount = 0;

    std::vector<ECS::Entity> terrainEntities;
    for (ECS::Entity root : rootEntities) {
        CollectTerrainEntities(root, terrainEntities);
    }

    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    std::unordered_set<ECS::Entity> activeEntities;
    activeEntities.reserve(terrainEntities.size());

    for (ECS::Entity entity : terrainEntities) {
        if (!coordinator.HasComponent<ECS::TerrainComponent>(entity) ||
            !coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            continue;
        }

        const ECS::TerrainComponent& settings = coordinator.GetComponent<ECS::TerrainComponent>(entity);
        // 不再要求 heightmapPath 非空：空路径表示程序化平坦高度图（编辑器新建默认值）。
        if (!settings.enabled) {
            continue;
        }

        Resource* resource = EnsureResource(entity, settings);
        if (!resource) {
            continue;
        }

        activeEntities.insert(entity);
        const glm::mat4 model = sceneECS.GetWorldMatrix(entity);
        const bool modelChanged = !SameMat4(resource->model, model);
        resource->model = model;
        if (modelChanged) {
            resource->grassCullCacheValid = false;
            resource->terrainMdiCullCacheValid = false;
        }
        resource->chunks.UpdateVisibility(cameraPosition, resource->model,
                                          frustumPlanes, useFrustumCulling, viewSlot);
        // 草剔除参考系缓存：只在"带视锥"的 Prepare 时更新。阴影 pass 的
        // noTerrainFrustum Prepare（useFrustumCulling=false）不得清掉主几何
        // 阶段写入的参考系，否则主 pass 剔除会退化成当前渲染视图平面——在
        // 编辑器"主相机剔除"模式下与地形 chunk 参考系分裂（游戏视锥外的草
        // 照画而地形已剔，2026-09-18 用户截图报告的问题）。
        // 反射探针（第三个视图）不写这份共享参考系：它的面相机是 1:1 纵横比的
        // 独立相机，写进去会让**下一帧首个**地形/草剔除用错视锥，实测把主视图
        // 地形候选从 98 个 tile 降到 56 个并把 terrainMdiTileCapacity 锁死在错值。
        // 探针自己按「现场平面」分支剔除即可（见 RecordTerrainGpuCull / 草路径）。
        if (useFrustumCulling && !IsProbeViewSlot(viewSlot)) {
            resource->grassFrustumPlanes = frustumPlanes;
            resource->grassUseFrustumCulling = true;
            resource->grassCameraPosition = cameraPosition;
            resource->terrainMdiFrustumPlanes = frustumPlanes;
            resource->terrainMdiUseFrustumCulling = true;
            resource->terrainMdiCameraPosition = cameraPosition;
        }
        m_PreparedResources.push_back(resource);
        m_VisibleChunkCount += resource->chunks.GetVisibleCount();
    }

    if (TerrainDiagEnabled()) {
        LOGI("[TerrainDiag] Prepare slot=%d frustum=%d cam=(%.2f,%.2f,%.2f) prepared=%zu visibleChunks=%d",
             viewSlot, useFrustumCulling ? 1 : 0, cameraPosition.x, cameraPosition.y,
             cameraPosition.z, m_PreparedResources.size(), m_VisibleChunkCount);
    }

    bool hasStaleResources = false;
    for (const auto& [entity, resource] : m_Resources) {
        if (activeEntities.find(entity) == activeEntities.end()) {
            hasStaleResources = true;
            break;
        }
    }
    if (hasStaleResources && g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    for (auto it = m_Resources.begin(); it != m_Resources.end();) {
        if (activeEntities.find(it->first) == activeEntities.end()) {
            if (it->second) {
                DestroyResource(*it->second);
            }
            it = m_Resources.erase(it);
        } else {
            ++it;
        }
    }
}

void TerrainRenderer::Prepare(const RenderWorld& world,
                              const glm::vec3& cameraPosition,
                              const std::array<Plane, 6>& frustumPlanes,
                              bool useFrustumCulling,
                              int viewSlot) {
    m_PreparedResources.clear();
    m_VisibleChunkCount = 0;

    std::unordered_set<ECS::Entity> activeEntities;
    activeEntities.reserve(world.terrains.size());

    for (const RenderTerrainData& terrain : world.terrains) {
        const RenderWorldEntity* entityData = world.Find(terrain.entity);
        if (entityData == nullptr || !entityData->visible || !entityData->hasTransform ||
            !terrain.enabled) {
            continue;
        }

        const ECS::TerrainComponent settings = MakeTerrainSettings(terrain);
        Resource* resource = EnsureResource(terrain.entity, settings);
        if (!resource) {
            continue;
        }

        activeEntities.insert(terrain.entity);
        const glm::mat4& model = entityData->transform.worldMatrix;
        const bool modelChanged = !SameMat4(resource->model, model);
        resource->model = model;
        if (modelChanged) {
            resource->grassCullCacheValid = false;
            resource->terrainMdiCullCacheValid = false;
        }
        resource->chunks.UpdateVisibility(cameraPosition, resource->model,
                                          frustumPlanes, useFrustumCulling, viewSlot);
        // 草剔除参考系缓存：只在"带视锥"的 Prepare 时更新。阴影 pass 的
        // noTerrainFrustum Prepare（useFrustumCulling=false）不得清掉主几何
        // 阶段写入的参考系，否则主 pass 剔除会退化成当前渲染视图平面——在
        // 编辑器"主相机剔除"模式下与地形 chunk 参考系分裂（游戏视锥外的草
        // 照画而地形已剔，2026-09-18 用户截图报告的问题）。
        // 反射探针（第三个视图）不写这份共享参考系：它的面相机是 1:1 纵横比的
        // 独立相机，写进去会让**下一帧首个**地形/草剔除用错视锥，实测把主视图
        // 地形候选从 98 个 tile 降到 56 个并把 terrainMdiTileCapacity 锁死在错值。
        // 探针自己按「现场平面」分支剔除即可（见 RecordTerrainGpuCull / 草路径）。
        if (useFrustumCulling && !IsProbeViewSlot(viewSlot)) {
            resource->grassFrustumPlanes = frustumPlanes;
            resource->grassUseFrustumCulling = true;
            resource->grassCameraPosition = cameraPosition;
            resource->terrainMdiFrustumPlanes = frustumPlanes;
            resource->terrainMdiUseFrustumCulling = true;
            resource->terrainMdiCameraPosition = cameraPosition;
        }
        m_PreparedResources.push_back(resource);
        m_VisibleChunkCount += resource->chunks.GetVisibleCount();
    }

    if (TerrainDiagEnabled()) {
        LOGI("[TerrainDiag] Prepare slot=%d frustum=%d cam=(%.2f,%.2f,%.2f) prepared=%zu visibleChunks=%d",
             viewSlot, useFrustumCulling ? 1 : 0, cameraPosition.x, cameraPosition.y,
             cameraPosition.z, m_PreparedResources.size(), m_VisibleChunkCount);
    }

    bool hasStaleResources = false;
    for (const auto& [entity, resource] : m_Resources) {
        if (activeEntities.find(entity) == activeEntities.end()) {
            hasStaleResources = true;
            break;
        }
    }
    if (hasStaleResources && g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    for (auto it = m_Resources.begin(); it != m_Resources.end();) {
        if (activeEntities.find(it->first) == activeEntities.end()) {
            if (it->second) {
                DestroyResource(*it->second);
            }
            it = m_Resources.erase(it);
        } else {
            ++it;
        }
    }
}

void TerrainRenderer::PrepareFromScene(const glm::vec3& cameraPosition,
                                       const std::array<Plane, 6>& frustumPlanes,
                                       bool useFrustumCulling,
                                       int viewSlot) {
    Prepare(ECS::SceneECS::GetInstance().GetRootEntities(), cameraPosition,
            frustumPlanes, useFrustumCulling, viewSlot);
}

void TerrainRenderer::CollectTerrainEntities(ECS::Entity entity,
                                              std::vector<ECS::Entity>& entities) const {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (coordinator.HasComponent<ECS::TerrainComponent>(entity)) {
        const auto& terrain = coordinator.GetComponent<ECS::TerrainComponent>(entity);
        const bool visible = !coordinator.HasComponent<ECS::RenderComponent>(entity) ||
                             coordinator.GetComponent<ECS::RenderComponent>(entity).visible;
        if (terrain.enabled && visible) {
            entities.push_back(entity);
        }
    }

    for (ECS::Entity child : ECS::SceneECS::GetInstance().GetChildren(entity)) {
        CollectTerrainEntities(child, entities);
    }
}

TerrainRenderer::Resource* TerrainRenderer::EnsureResource(
    ECS::Entity entity, const ECS::TerrainComponent& settings) {
    auto it = m_Resources.find(entity);
    if (it != m_Resources.end() && it->second && SettingsEqual(it->second->settings, settings)) {
        // wireframe 只影响管线选择、不参与 SettingsEqual，避免切换开关触发整资源重建；
        // 这里直接同步最新值，渲染循环按它选择线框/实体管线。
        it->second->settings.wireframe = settings.wireframe;
        return it->second.get();
    }

    if (it != m_Resources.end()) {
        // 编辑器修改路径或布局时旧 descriptor/纹理可能仍被本帧 GPU 使用。
        if (g_Device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_Device);
        }
        if (it->second) {
            DestroyResource(*it->second);
        }
        m_Resources.erase(it);
    }

    std::unique_ptr<Resource> resource = CreateResource(entity, settings);
    if (!resource) {
        return nullptr;
    }
    Resource* result = resource.get();
    m_Resources.emplace(entity, std::move(resource));
    return result;
}

std::unique_ptr<TerrainRenderer::Resource> TerrainRenderer::CreateResource(
    ECS::Entity entity, const ECS::TerrainComponent& settings) {
    if (!g_TexturePool || !EnsureWhiteFallback()) {
        return nullptr;
    }

    auto resource = std::make_unique<Resource>();
    resource->entity = entity;
    resource->settings = settings;
    resource->heightmapKey = MakeTextureKey(entity, "height");

    // 高度图来源优先级：雕刻产物 > 原始资产（空 = 程序化平坦）。两条路径
    // 共同点是都保留一份 CPU 镜像：地形笔刷只改镜像再局部上传，因此不需要
    // PNG 编码器，也不会因为改像素而触发资源重建（SettingsEqual 只看路径）。
    // 显式保存后 sculptedHeightmapPath 非空，重载即从产物 PNG 读回修改。
    std::string effectiveHeightmapPath = settings.heightmapPath;
    if (!settings.sculptedHeightmapPath.empty()) {
        effectiveHeightmapPath = settings.sculptedHeightmapPath;
    }
    if (effectiveHeightmapPath.empty()) {
        resource->heightmapProcedural = true;
        resource->heightmapWidth = kProceduralHeightmapResolution;
        resource->heightmapHeight = kProceduralHeightmapResolution;
        resource->heightmapCpu.assign(
            static_cast<size_t>(resource->heightmapWidth) * resource->heightmapHeight,
            kProceduralFlatSample);
        if (!g_TexturePool->CreateHeightmap16FromMemory(
                resource->heightmapKey, resource->heightmapWidth, resource->heightmapHeight,
                resource->heightmapCpu.data(), SamplerType::LinearClamp)) {
            LOGE("[TerrainRenderer] failed to create procedural flat heightmap for entity %u",
                        static_cast<unsigned>(entity));
            return nullptr;
        }
    } else {
        const std::string heightmapPath = EngineConfig::GetFullPath(effectiveHeightmapPath.c_str());
        HeightmapPixels16 pixels;
        std::string errorMessage;
        if (!HeightmapLoader::LoadPng16(heightmapPath, pixels, &errorMessage)) {
            // 产物加载失败回退原始资产，宁可丢修改也不要整块地形消失。
            if (!settings.sculptedHeightmapPath.empty() &&
                !settings.heightmapPath.empty()) {
                LOGW("[TerrainRenderer] sculpted heightmap '%s' failed for entity %u (%s), "
                            "falling back to source heightmap '%s'",
                            settings.sculptedHeightmapPath.c_str(), static_cast<unsigned>(entity),
                            errorMessage.c_str(), settings.heightmapPath.c_str());
                const std::string fallbackPath =
                    EngineConfig::GetFullPath(settings.heightmapPath.c_str());
                if (HeightmapLoader::LoadPng16(fallbackPath, pixels, &errorMessage)) {
                    effectiveHeightmapPath = settings.heightmapPath;
                }
            }
            if (!pixels.IsValid()) {
                LOGE("[TerrainRenderer] failed to load heightmap for entity %u: %s (%s)",
                            static_cast<unsigned>(entity), effectiveHeightmapPath.c_str(),
                            errorMessage.c_str());
                return nullptr;
            }
        }
        resource->heightmapWidth = pixels.width;
        resource->heightmapHeight = pixels.height;
        resource->heightmapCpu = std::move(pixels.samples);
        if (!g_TexturePool->CreateHeightmap16FromMemory(
                resource->heightmapKey, resource->heightmapWidth, resource->heightmapHeight,
                resource->heightmapCpu.data(), SamplerType::LinearClamp)) {
            LOGE("[TerrainRenderer] failed to upload heightmap for entity %u: %s",
                        static_cast<unsigned>(entity), effectiveHeightmapPath.c_str());
            return nullptr;
        }
    }
    resource->ownedTextureKeys.push_back(resource->heightmapKey);

    for (int layer = 0; layer < 4; ++layer) {
        const std::string& path = GetLayerPath(settings, layer);
        resource->layerKeys[static_cast<size_t>(layer)] = "white";
        if (path.empty()) {
            continue;
        }

        const std::string slot = "layer" + std::to_string(layer);
        const std::string key = MakeTextureKey(entity, slot.c_str());
        const std::string layerPath = EngineConfig::GetFullPath(path.c_str());
        if (g_TexturePool->LoadTexture2D(key, layerPath, SamplerType::LinearRepeat) &&
            g_TexturePool->GetTexture(key)) {
            resource->layerKeys[static_cast<size_t>(layer)] = key;
            resource->ownedTextureKeys.push_back(key);
        } else {
            // 工程内没有时回退到引擎自带材质（如「高度图地形」预设引用的
            // terrain/prototype/materials/*，随引擎分发，任何工程都可用）。
            const std::string enginePath = EngineConfig::GetEngineTexturePath(path.c_str());
            bool loadedFromEngine = false;
            if (enginePath != layerPath) {
                loadedFromEngine =
                    g_TexturePool->LoadTexture2D(key, enginePath, SamplerType::LinearRepeat) &&
                    g_TexturePool->GetTexture(key);
            }
            if (loadedFromEngine) {
                resource->layerKeys[static_cast<size_t>(layer)] = key;
                resource->ownedTextureKeys.push_back(key);
                LOGI("[TerrainRenderer] layer %d for entity %u not found in project, "
                            "loaded from engine assets: %s",
                            layer, static_cast<unsigned>(entity), path.c_str());
            } else {
                LOGE("[TerrainRenderer] layer %d failed for entity %u, using white fallback: %s",
                            layer, static_cast<unsigned>(entity), path.c_str());
            }
        }
    }

    // 控制图 = 图层权重图，也是材质笔刷的写入目标，所以和高度图一样尽量留 CPU 镜像。
    //   路径为空             → 按坡度规则生成程序化控制图（与着色器自动混合等价，外观不跳变）
    //   路径非空 && 可 CPU 解码 → 用镜像建纹理，材质笔刷可涂
    //   路径非空 && 不可解码    → 例如 KTX2/DDS 压缩格式：退回原来的纹理加载路径，只读
    resource->controlKey = "white";
    {
        const std::string controlKey = MakeTextureKey(entity, "control");
        bool buildFromMemory = false;

        // 加载优先级：涂色产物 > 原始控制图 > 程序化坡度混合。
        std::string controlSource = settings.controlMapPath;
        if (!settings.paintedControlMapPath.empty()) {
            controlSource = settings.paintedControlMapPath;
        }

        if (controlSource.empty()) {
            resource->controlProcedural = true;
            buildFromMemory = true;
        } else {
            const std::string controlPath = EngineConfig::GetFullPath(controlSource.c_str());
            resource->controlProcedural = false;
            if (g_TexturePool->LoadControlMapPixels8(controlPath, resource->controlWidth,
                                                     resource->controlHeight,
                                                     resource->controlCpu)) {
                buildFromMemory = true;
            } else if (g_TexturePool->LoadTexture2D(controlKey, controlPath,
                                                    SamplerType::LinearClamp) &&
                       g_TexturePool->GetTexture(controlKey)) {
                // 压缩纹理在 CPU 侧解不开，拿不到镜像 ⇒ 这个地形的材质笔刷只读。
                resource->controlKey = controlKey;
                resource->ownedTextureKeys.push_back(controlKey);
                resource->useControlMap = true;
                resource->controlCpu.clear();
                resource->controlWidth = 0;
                resource->controlHeight = 0;
                LOGW("[TerrainRenderer] control map '%s' for entity %u is not CPU-decodable, "
                            "material brush will be read-only for this terrain",
                            settings.controlMapPath.c_str(), static_cast<unsigned>(entity));
            } else {
                LOGE("[TerrainRenderer] control map '%s' failed for entity %u, "
                            "falling back to the procedural slope blend",
                            settings.controlMapPath.c_str(), static_cast<unsigned>(entity));
                resource->controlProcedural = true;
                buildFromMemory = true;
            }
        }

        if (buildFromMemory) {
            if (resource->controlProcedural) {
                resource->controlWidth = resource->heightmapWidth;
                resource->controlHeight = resource->heightmapHeight;
                ComputeSlopeBlendWeights(resource->heightmapCpu.data(),
                                         resource->heightmapWidth, resource->heightmapHeight,
                                         settings.worldSize, settings.heightScale,
                                         resource->controlCpu);
            }

            if (g_TexturePool->CreateControlMap8FromMemory(controlKey, resource->controlWidth,
                                                           resource->controlHeight,
                                                           resource->controlCpu.data(),
                                                           SamplerType::LinearClamp)) {
                resource->controlKey = controlKey;
                resource->ownedTextureKeys.push_back(controlKey);
                resource->useControlMap = true;
            } else {
                // 退回到着色器的自动坡度混合；同时丢掉镜像，笔刷据此判定"不可涂抹"。
                LOGE("[TerrainRenderer] failed to upload control map for entity %u, "
                            "material brush will be unavailable",
                            static_cast<unsigned>(entity));
                resource->controlCpu.clear();
                resource->controlWidth = 0;
                resource->controlHeight = 0;
            }
        }
    }

    // 草密度图（R8，与高度图同分辨率）：默认零初始化（无草）；显式保存过的
    // 场景从产物 PNG 读回（取灰度通道），草地笔刷只改 CPU 镜像再局部回写。
    resource->grassKey = MakeTextureKey(entity, "grass");
    resource->grassWidth = resource->heightmapWidth;
    resource->grassHeight = resource->heightmapHeight;
    resource->grassCpu.assign(static_cast<size_t>(resource->grassWidth) *
                                  resource->grassHeight, 0);
    if (!settings.paintedGrassPath.empty()) {
        const std::string grassPath = EngineConfig::GetFullPath(settings.paintedGrassPath.c_str());
        uint32_t loadedW = 0;
        uint32_t loadedH = 0;
        std::vector<uint8_t> rgba;
        if (g_TexturePool->LoadControlMapPixels8(grassPath, loadedW, loadedH, rgba) &&
            loadedW == resource->grassWidth && loadedH == resource->grassHeight) {
            for (size_t i = 0; i < resource->grassCpu.size(); ++i) {
                resource->grassCpu[i] = rgba[i * 4]; // 取灰度 R 通道
            }
            LOGI("[TerrainRenderer] loaded painted grass map for entity %u: %s",
                        static_cast<unsigned>(entity), settings.paintedGrassPath.c_str());
        } else {
            LOGW("[TerrainRenderer] painted grass map '%s' for entity %u missing or "
                        "resolution mismatch (%ux%u vs %ux%u), starting with no grass",
                        settings.paintedGrassPath.c_str(), static_cast<unsigned>(entity),
                        loadedW, loadedH, resource->grassWidth, resource->grassHeight);
        }
    }
    resource->grassDirty = true;
    if (!g_TexturePool->CreateGrassMask8FromMemory(resource->grassKey,
                                                   resource->grassWidth,
                                                   resource->grassHeight,
                                                   resource->grassCpu.data(),
                                                   SamplerType::LinearClamp)) {
        // 密度图建不出来只影响草地渲染，地形本体照常工作。
        LOGE("[TerrainRenderer] grass mask creation failed for entity %u",
                    static_cast<unsigned>(entity));
        resource->grassKey.clear();
        resource->grassCpu.clear();
    } else {
        resource->ownedTextureKeys.push_back(resource->grassKey);
    }

    // 水位图（R8，与高度图同分辨率）：默认零初始化（无水）；显式保存过的
    // 场景从产物 PNG 读回（取灰度通道），水位笔刷只改 CPU 镜像再局部回写。
    // 渲染侧目前是地形片元着色器里的不透明水面 mask（占位实现）。
    resource->waterKey = MakeTextureKey(entity, "water");
    resource->waterWidth = resource->heightmapWidth;
    resource->waterHeight = resource->heightmapHeight;
    resource->waterCpu.assign(static_cast<size_t>(resource->waterWidth) *
                                  resource->waterHeight, 0);
    if (!settings.paintedWaterPath.empty()) {
        const std::string waterPath = EngineConfig::GetFullPath(settings.paintedWaterPath.c_str());
        uint32_t loadedW = 0;
        uint32_t loadedH = 0;
        std::vector<uint8_t> rgba;
        if (g_TexturePool->LoadControlMapPixels8(waterPath, loadedW, loadedH, rgba) &&
            loadedW == resource->waterWidth && loadedH == resource->waterHeight) {
            for (size_t i = 0; i < resource->waterCpu.size(); ++i) {
                resource->waterCpu[i] = rgba[i * 4]; // 取灰度 R 通道
            }
            LOGI("[TerrainRenderer] loaded painted water map for entity %u: %s",
                        static_cast<unsigned>(entity), settings.paintedWaterPath.c_str());
        } else {
            LOGW("[TerrainRenderer] painted water map '%s' for entity %u missing or "
                        "resolution mismatch (%ux%u vs %ux%u), starting with no water",
                        settings.paintedWaterPath.c_str(), static_cast<unsigned>(entity),
                        loadedW, loadedH, resource->waterWidth, resource->waterHeight);
        }
    }
    if (!g_TexturePool->CreateGrassMask8FromMemory(resource->waterKey,
                                                   resource->waterWidth,
                                                   resource->waterHeight,
                                                   resource->waterCpu.data(),
                                                   SamplerType::LinearClamp)) {
        // 水位图建不出来只影响水面显示，地形本体照常工作。
        LOGE("[TerrainRenderer] water mask creation failed for entity %u",
                    static_cast<unsigned>(entity));
        resource->waterKey.clear();
        resource->waterCpu.clear();
    } else {
        resource->ownedTextureKeys.push_back(resource->waterKey);
    }

    const float height0 = settings.heightOffset;
    const float height1 = settings.heightOffset + settings.heightScale;
    resource->chunks.Configure(settings.worldSize, settings.chunkCount,
                               std::min(height0, height1), std::max(height0, height1),
                               settings.viewDistance, settings.lod0Distance,
                               settings.lod1Distance, settings.maxLod);

    int baseResolution = std::clamp(settings.patchResolution, 3, 129);
    if ((baseResolution & 1) == 0) {
        --baseResolution;
    }
    for (int lod = 0; lod < 3; ++lod) {
        const uint32_t resolution = static_cast<uint32_t>(((baseResolution - 1) >> lod) + 1);
        if (!BuildPatch(resource->patches[static_cast<size_t>(lod)], resolution)) {
            DestroyResource(*resource);
            return nullptr;
        }

        const uint32_t sourceIntervals = resolution - 1u;
        const uint32_t tileIntervals = std::max(
            1u, (sourceIntervals + kTerrainMdiTileSubdivision - 1u) /
                    kTerrainMdiTileSubdivision);
        const uint32_t tileResolution = tileIntervals + 1u;
        if (!BuildPatch(resource->terrainMdiPatches[static_cast<size_t>(lod)],
                        tileResolution)) {
            DestroyResource(*resource);
            return nullptr;
        }
    }

    // 先用小容量启动，按本帧实际可见 Chunk 增长，避免 chunkCount=256
    // 时为每个地形预先分配 65536 个实例槽位。
    constexpr size_t kInitialInstanceCapacity = 64;
    const size_t chunkCapacity = resource->chunks.GetChunks().size();
    const size_t initialInstanceCapacity = std::max<size_t>(
        1, std::min(chunkCapacity, kInitialInstanceCapacity));
    if (!CreateInstanceBuffers(*resource, initialInstanceCapacity) ||
        !CreateUniformBuffers(*resource) ||
        !CreateDescriptorSets(*resource)) {
        DestroyResource(*resource);
        return nullptr;
    }

    // 水面网格：覆盖整块地形的静态 UV 网格，顶点高度由 terrain_water.vert
    // 按高度图+水位图现场计算，涂水/挖地形只更新纹理、网格永不重建。
    // 分辨率刻意压到 32（约 16m/格、2k 三角形）：水面是静止平面，无波纹
    // 无细节需求；唯一约束是"水深在顶点处采样"，格子边长决定可涂出的
    // 最小水域——比格子还小的笔刷点会落在顶点之间而完全丢失。再想合并
    // 只能改屏空间水面或逐片元采样，成本另算。失败仅降级为"无水面网格"。
    if (m_WaterTargetPipeline.GetPipeline() != VK_NULL_HANDLE) {
        if (!BuildPatch(resource->waterPatch, 32)) {
            LOGE("[TerrainRenderer] water surface grid build failed - water surface disabled");
            resource->waterPatch.Cleanup();
        }
    }

    return resource;
}

void TerrainRenderer::DestroyResource(Resource& resource) {
    if (m_DescriptorPool != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        std::vector<VkDescriptorSet> sets;
        sets.reserve(resource.descriptorSets.size() +
                     static_cast<size_t>(kFramesInFlight) * kProbeFaceCount);
        for (VkDescriptorSet& set : resource.descriptorSets) {
            if (set != VK_NULL_HANDLE) {
                sets.push_back(set);
                set = VK_NULL_HANDLE;
            }
        }
        for (auto* viewSets : {&resource.gameDescriptorSets, &resource.shadowDescriptorSets}) {
            for (VkDescriptorSet& set : *viewSets) {
                if (set != VK_NULL_HANDLE) {
                    sets.push_back(set);
                    set = VK_NULL_HANDLE;
                }
            }
        }
        // 探针 6 面各一套（见 kProbeFaceCount），与主集同一 pool，必须一并回收，
        // 否则每次资源重建都会泄漏 12 套（2 帧 × 6 面）。
        for (auto& frameSets : resource.probeDescriptorSets) {
            for (VkDescriptorSet& set : frameSets) {
                if (set != VK_NULL_HANDLE) {
                    sets.push_back(set);
                    set = VK_NULL_HANDLE;
                }
            }
        }
        if (!sets.empty()) {
            vkFreeDescriptorSets(g_Device, m_DescriptorPool,
                                 static_cast<uint32_t>(sets.size()), sets.data());
        }
    }

    for (auto& uniform : resource.uniformBuffers) {
        uniform.reset();
    }
    for (auto* viewUbos : {&resource.gameUniformBuffers, &resource.shadowUniformBuffers}) {
        for (auto& uniform : *viewUbos) uniform.reset();
    }
    for (auto& frameUbos : resource.probeUniformBuffers) {
        for (auto& probeUbo : frameUbos) {
            probeUbo.reset();
        }
    }
    for (auto& instanceBuffer : resource.instanceBuffers) {
        instanceBuffer.Cleanup();
    }
    for (auto& frameBuffers : resource.probeInstanceBuffers) {
        for (auto& instanceBuffer : frameBuffers) {
            instanceBuffer.Cleanup();
        }
    }
    resource.probeInstanceCapacity = 0;
    for (auto& instanceBuffer : resource.csmInstanceBuffers) {
        instanceBuffer.Cleanup();
    }
    resource.csmInstanceCapacity = 0;
    for (auto& grassBuffer : resource.grassInstanceBuffers) {
        grassBuffer.Cleanup();
    }
    resource.grassInstanceCapacity = 0;
    resource.grassInstanceCount = 0;
    // 草 GPU 剔除缓冲（追加在尾部）：桶 SSBO + 间接命令 SSBO + 剔除描述符。
    for (auto& cullBuffer : resource.grassBucketGpuBuffers) {
        cullBuffer.Cleanup();
    }
    for (auto& cullBuffer : resource.grassIndirectBuffers) {
        cullBuffer.Cleanup();
    }
    resource.grassGpuBucketCapacity = 0;
    resource.grassGpuBucketTotal = 0;
    if (m_GrassCullDescriptorPool != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        std::vector<VkDescriptorSet> cullSets;
        cullSets.reserve(resource.grassCullDescriptorSets.size());
        for (VkDescriptorSet& set : resource.grassCullDescriptorSets) {
            if (set != VK_NULL_HANDLE) {
                cullSets.push_back(set);
                set = VK_NULL_HANDLE;
            }
        }
        if (!cullSets.empty()) {
            vkFreeDescriptorSets(g_Device, m_GrassCullDescriptorPool,
                                 static_cast<uint32_t>(cullSets.size()), cullSets.data());
        }
    }
    // 叶片级剔除缓冲与描述符（追加在尾部）。
    for (auto& compactBuffer : resource.grassBladeCompactBuffers) {
        compactBuffer.Cleanup();
    }
    for (auto& bladeCmdBuffer : resource.grassBladeCmdBuffers) {
        bladeCmdBuffer.Cleanup();
    }
    for (auto& bladeParamsBuffer : resource.grassBladeParamsBuffers) {
        bladeParamsBuffer.Cleanup();
    }
    for (auto& bucketListBuffer : resource.grassBladeBucketListBuffers) {
        bucketListBuffer.Cleanup();
    }
    resource.grassBladeCompactCapacity = 0;
    if (m_GrassBladeCullDescriptorPool != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        std::vector<VkDescriptorSet> bladeSets;
        bladeSets.reserve(kFramesInFlight * kGrassCullViewSlots);
        for (auto& frameSets : resource.grassBladeCullDescriptorSets) {
            for (VkDescriptorSet& set : frameSets) {
                if (set != VK_NULL_HANDLE) {
                    bladeSets.push_back(set);
                    set = VK_NULL_HANDLE;
                }
            }
        }
        if (!bladeSets.empty()) {
            vkFreeDescriptorSets(g_Device, m_GrassBladeCullDescriptorPool,
                                 static_cast<uint32_t>(bladeSets.size()), bladeSets.data());
        }
    }

    // 地形 GPU MDI 缓冲与描述符（追加在 Resource 尾部，CPU chunk 路径不依赖）。
    for (auto& instanceBuffer : resource.terrainMdiInstanceBuffers) {
        instanceBuffer.Cleanup();
    }
    for (auto& compactBuffer : resource.terrainMdiCompactInstanceBuffers) {
        compactBuffer.Cleanup();
    }
    for (auto& tileBuffer : resource.terrainMdiTileBuffers) {
        tileBuffer.Cleanup();
    }
    for (auto& indirectBuffer : resource.terrainMdiIndirectBuffers) {
        indirectBuffer.Cleanup();
    }
    for (auto& viewBuffer : resource.terrainMdiCullViewBuffers) {
        viewBuffer.Cleanup();
    }
    resource.terrainMdiTileCapacity = 0;
    resource.terrainMdiUploadCaches = {};
    resource.terrainMdiLodCaches = {};
    ++resource.terrainMdiCandidateRevision;
    resource.terrainMdiCullClearedFrame = UINT64_MAX;
    resource.terrainMdiTileCount = 0;
    resource.terrainMdiTileCounts.fill(0);
    resource.terrainMdiGridCount = 0;
    if (m_TerrainMdiCullDescriptorPool != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        std::vector<VkDescriptorSet> terrainSets;
        terrainSets.reserve(resource.terrainMdiCullDescriptorSets.size());
        for (VkDescriptorSet& set : resource.terrainMdiCullDescriptorSets) {
            if (set != VK_NULL_HANDLE) {
                terrainSets.push_back(set);
                set = VK_NULL_HANDLE;
            }
        }
        if (!terrainSets.empty()) {
            vkFreeDescriptorSets(g_Device, m_TerrainMdiCullDescriptorPool,
                                 static_cast<uint32_t>(terrainSets.size()), terrainSets.data());
        }
    }
    for (auto& patch : resource.patches) {
        patch.Cleanup();
    }
    for (auto& patch : resource.terrainMdiPatches) {
        patch.Cleanup();
    }
    resource.waterPatch.Cleanup();

    if (g_TexturePool) {
        for (const std::string& key : resource.ownedTextureKeys) {
            g_TexturePool->Release(key);
        }
    }
    resource.ownedTextureKeys.clear();
}

bool TerrainRenderer::SettingsEqual(const ECS::TerrainComponent& lhs,
                                    const ECS::TerrainComponent& rhs) const {
    return lhs.enabled == rhs.enabled &&
           lhs.heightmapPath == rhs.heightmapPath &&
           lhs.worldSize.x == rhs.worldSize.x && lhs.worldSize.y == rhs.worldSize.y &&
           lhs.heightScale == rhs.heightScale && lhs.heightOffset == rhs.heightOffset &&
           lhs.chunkCount == rhs.chunkCount && lhs.patchResolution == rhs.patchResolution &&
           lhs.viewDistance == rhs.viewDistance &&
           lhs.lod0Distance == rhs.lod0Distance && lhs.lod1Distance == rhs.lod1Distance &&
           lhs.maxLod == rhs.maxLod && lhs.materialTiling == rhs.materialTiling &&
           lhs.blendSharpness == rhs.blendSharpness &&
           lhs.layer0Path == rhs.layer0Path && lhs.layer1Path == rhs.layer1Path &&
           lhs.layer2Path == rhs.layer2Path && lhs.layer3Path == rhs.layer3Path &&
           lhs.controlMapPath == rhs.controlMapPath;
}

bool TerrainRenderer::EnsureWhiteFallback() {
    if (!g_TexturePool) {
        return false;
    }
    if (g_TexturePool->GetTexture("white")) {
        return true;
    }

    const std::string materialPath = EngineConfig::GetEngineTexturePath("material.png");
    if (!g_TexturePool->LoadTexture2D("white", materialPath, SamplerType::LinearRepeat)) {
        LOGE("[TerrainRenderer] unable to create white texture fallback: %s", materialPath.c_str());
        return false;
    }
    m_OwnedWhiteFallback = true;
    return g_TexturePool->GetTexture("white") != nullptr;
}

bool TerrainRenderer::CreateDescriptorResources() {
    if (m_DescriptorLayout != VK_NULL_HANDLE && m_DescriptorPool != VK_NULL_HANDLE) {
        return true;
    }
    if (g_Device == VK_NULL_HANDLE) {
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        if (i == 1 || i == 7) {
            // 高度图/水位图同时供顶点位移使用（地形/水面网格顶点抬升）。
            bindings[i].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
        }
    }
    // binding 7 = 水位图（R8）：terrain_water.vert 顶点抬升 + 仅片元采样兼容。

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_DescriptorLayout) != VK_SUCCESS) {
        m_DescriptorLayout = VK_NULL_HANDLE;
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    // 每个地形资源同时保留主视图和全景捕获两套 descriptor/UBO。
    const uint32_t descriptorSetCapacity = static_cast<uint32_t>(kInitialDescriptorSets * 2);
    poolSizes[0].descriptorCount = descriptorSetCapacity;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = descriptorSetCapacity * 7;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = descriptorSetCapacity;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_DescriptorPool) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorLayout, g_Allocator);
        m_DescriptorLayout = VK_NULL_HANDLE;
        m_DescriptorPool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void TerrainRenderer::DestroyDescriptorResources() {
    if (g_Device == VK_NULL_HANDLE) {
        m_DescriptorPool = VK_NULL_HANDLE;
        m_DescriptorLayout = VK_NULL_HANDLE;
        return;
    }
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    if (m_DescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorLayout, g_Allocator);
        m_DescriptorLayout = VK_NULL_HANDLE;
    }
}

bool TerrainRenderer::CreatePipelines() {
    if (m_RenderPass == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE) {
        return false;
    }

    const std::array<VkVertexInputBindingDescription, 2> vertexBindings = {
        MakeVertexBinding(0, sizeof(TerrainVertex), VK_VERTEX_INPUT_RATE_VERTEX),
        MakeVertexBinding(1, sizeof(TerrainChunkInstance), VK_VERTEX_INPUT_RATE_INSTANCE)
    };
    const std::array<VkVertexInputAttributeDescription, 4> vertexAttributes = {
        MakeVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, position)),
        MakeVertexAttribute(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, originSize)),
        MakeVertexAttribute(2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, uvRect)),
        MakeVertexAttribute(3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, params))
    };

    // Vulkan 的 primitiveRestartEnable 不通过 VkPhysicalDeviceFeatures 暴露；
    // 对 triangle strip 是核心输入装配能力。
    m_PrimitiveRestartSupported = true;

    PipelineConfig geometryConfig;
    geometryConfig.vertShader = "terrain.vert.spv";
    geometryConfig.fragShader = "terrain.frag.spv";
    geometryConfig.topology = m_PrimitiveRestartSupported
        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    geometryConfig.primitiveRestartEnable = m_PrimitiveRestartSupported;
    geometryConfig.cullMode = VK_CULL_MODE_BACK_BIT;
    geometryConfig.depthTest = true;
    geometryConfig.depthWrite = !g_EnableZPrepass;
    geometryConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    geometryConfig.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    geometryConfig.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT,
        0
    };
    geometryConfig.subpass = 1;
    geometryConfig.vertexBindings.assign(vertexBindings.begin(), vertexBindings.end());
    geometryConfig.vertexAttributes.assign(vertexAttributes.begin(), vertexAttributes.end());
    if (!m_Pipeline.Create(m_RenderPass, m_DescriptorLayout, geometryConfig)) {
        return false;
    }

    if (!g_UseSeparateMrtRenderPass) {
        PipelineConfig depthConfig = geometryConfig;
        depthConfig.vertShader = "terrain_depth.vert.spv";
        depthConfig.fragShader = "terrain_depth.frag.spv";
        depthConfig.depthWrite = true;
        depthConfig.depthCompareOp = VK_COMPARE_OP_LESS;
        depthConfig.colorAttachmentCount = kMainMrtZPrepassColorAttachmentCount;
        depthConfig.colorWriteMasks = { 0 };
        depthConfig.subpass = 0;
        if (!m_DepthPipeline.Create(m_RenderPass, m_DescriptorLayout, depthConfig)) {
            m_Pipeline.Cleanup();
            return false;
        }
    }

    // 线框模式管线（VK_POLYGON_MODE_LINE，需要设备特性 fillModeNonSolid）。
    // 关闭背面剔除，避免反面 patch 的线框缺失；创建失败仅降级为实体渲染，
    // 不影响常规管线（例如不支持 fillModeNonSolid 的移动端设备）。
    PipelineConfig wireframeConfig = geometryConfig;
    wireframeConfig.polygonMode = VK_POLYGON_MODE_LINE;
    wireframeConfig.cullMode = VK_CULL_MODE_NONE;
    if (!m_WireframePipeline.Create(m_RenderPass, m_DescriptorLayout, wireframeConfig)) {
        LOGE("[TerrainRenderer] wireframe pipeline creation failed - wireframe mode disabled");
    }

    // 草地管线：无顶点缓冲，binding 0 即实例流（INSTANCE rate）；
    // 叶片几何由顶点着色器从二次贝塞尔曲线程序化生成（triangle strip）。
    // 草是不透明细三角形（无 alpha 混合），可直接写进地形所在的 G-buffer subpass；
    // 双面渲染（cullMode NONE），避免背面剔除把朝向随机的叶片剔除掉。
    {
        const std::array<VkVertexInputBindingDescription, 1> grassBindings = {
            MakeVertexBinding(0, sizeof(GrassBladeInstance), VK_VERTEX_INPUT_RATE_INSTANCE)
        };
        const std::array<VkVertexInputAttributeDescription, 2> grassAttributes = {
            MakeVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GrassBladeInstance, posParams)),
            MakeVertexAttribute(1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GrassBladeInstance, shapeParams))
        };
        PipelineConfig grassConfig;
        grassConfig.vertShader = "grass.vert.spv";
        grassConfig.fragShader = "grass.frag.spv";
        grassConfig.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        grassConfig.primitiveRestartEnable = false;
        grassConfig.cullMode = VK_CULL_MODE_NONE;
        grassConfig.depthTest = true;
        // Grass must remain in the main depth attachment for AO and post
        // processing, but its dedicated Hi-Z occluder color output is masked
        // off so blades cannot become occluders for terrain MDI.
        grassConfig.depthWrite = true;
        grassConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        // grass.vert 双模式：主 pass 用 UBO 的 projView，阴影 pass 用 push
        // constant 的级联矩阵（一帧内多次 UBO 写只有最后一次生效，级联矩阵
        // 不能走 UBO——与 terrain_csm_depth.vert 同一约定）。
        grassConfig.usePushConstants = true;
        grassConfig.pushConstantRange = {VK_SHADER_STAGE_VERTEX_BIT, 0,
                                         sizeof(glm::mat4) + sizeof(glm::vec4)};
        grassConfig.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
        grassConfig.colorWriteMasks = geometryConfig.colorWriteMasks;
        grassConfig.colorWriteMasks[4] = 0;
        grassConfig.subpass = 1;
        grassConfig.vertexBindings.assign(grassBindings.begin(), grassBindings.end());
        grassConfig.vertexAttributes.assign(grassAttributes.begin(), grassAttributes.end());
        if (!m_GrassPipeline.Create(m_RenderPass, m_DescriptorLayout, grassConfig)) {
            // 草地管线失败只降级草地渲染，不影响地形本体。
            LOGE("[TerrainRenderer] grass pipeline creation failed - grass rendering disabled");
        }
    }

    // 水面网格管线已迁移：地形水不再写 G-buffer（deferred water compositing），
    // EnsureWaterTargetPipeline 按需创建独立目标 RT 管线（共享 WaterTargetRT）。
    return true;
}

// 水面目标 RT 管线（deferred water compositing，2026-09-19）：地形涂刷水不再
// 写 G-buffer/主深度（那会覆盖水底几何），改画进共享 WaterTargetRT
//（R=mask G=线性视距(m) BA=八面体法线）。顶点阶段复用 terrain_water.vert
//（高度 = 地形 + 水位图抬升 + 岸线过渡），片元只写目标 RT。
bool TerrainRenderer::EnsureWaterTargetPipeline(VkRenderPass waterTargetRenderPass) {
    if (waterTargetRenderPass == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE) {
        return false;
    }
    if (m_WaterTargetPipeline.GetPipeline() != VK_NULL_HANDLE &&
        m_WaterTargetRenderPass == waterTargetRenderPass) {
        return true;
    }

    m_WaterTargetPipeline.Cleanup();

    const std::array<VkVertexInputBindingDescription, 1> waterBindings = {
        MakeVertexBinding(0, sizeof(glm::vec2), VK_VERTEX_INPUT_RATE_VERTEX)
    };
    const std::array<VkVertexInputAttributeDescription, 1> waterAttributes = {
        MakeVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0)
    };
    PipelineConfig config;
    config.vertShader = "terrain_water.vert.spv";
    config.fragShader = "terrain_water_target.frag.spv";
    config.topology = m_PrimitiveRestartSupported
        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.primitiveRestartEnable = m_PrimitiveRestartSupported;
    config.cullMode = VK_CULL_MODE_NONE;
    config.depthTest = true;
    config.depthWrite = true;
    config.depthCompareOp = VK_COMPARE_OP_LESS;
    config.blending = false;
    config.colorAttachmentCount = 1;
    config.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    config.subpass = 0;
    config.vertexBindings.assign(waterBindings.begin(), waterBindings.end());
    config.vertexAttributes.assign(waterAttributes.begin(), waterAttributes.end());
    if (!m_WaterTargetPipeline.Create(waterTargetRenderPass, m_DescriptorLayout, config)) {
        LOGE("[TerrainRenderer] water target pipeline creation failed");
        return false;
    }
    m_WaterTargetRenderPass = waterTargetRenderPass;
    return true;
}

// 在共享 WaterTargetRT 的活动 render pass 内绘制各 prepared 地形的水面网格。
// per-frame UBO 已由本帧 RenderInternal 写入（同一 projView），直接绑用；
// 干区片元由片元级水位守门剔除，岸边越界由合成端双深度比较兜底。
void TerrainRenderer::DrawWaterToTarget(VkCommandBuffer commandBuffer, int width, int height,
                                        const glm::mat4& projView, int viewSlot) {
    (void)projView;
    if (commandBuffer == VK_NULL_HANDLE || width <= 0 || height <= 0 ||
        m_PreparedResources.empty() ||
        m_WaterTargetPipeline.GetPipeline() == VK_NULL_HANDLE) {
        return;
    }

    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_WaterTargetPipeline.GetPipeline());
    for (Resource* resource : m_PreparedResources) {
        if (!resource || resource->waterPatch.indexCount == 0) {
            continue;
        }
        const VkDescriptorSet viewSet = ViewDescriptorSet(*resource, frame, viewSlot, 0);
        if (viewSet == VK_NULL_HANDLE) continue;
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_WaterTargetPipeline.GetLayout(), 0, 1,
                                &viewSet, 0, nullptr);
        VkDeviceSize waterOffset = 0;
        VkBuffer waterVertexBuffer = resource->waterPatch.vertexBuffer.GetBuffer();
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &waterVertexBuffer, &waterOffset);
        vkCmdBindIndexBuffer(commandBuffer,
                             resource->waterPatch.indexBuffer.GetBuffer(),
                             0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commandBuffer, resource->waterPatch.indexCount, 1, 0, 0, 0);
    }
}

bool TerrainRenderer::EnsureCsmDepthPipeline(VkRenderPass shadowRenderPass) {
    if (shadowRenderPass == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE) {
        return false;
    }
    if (m_CsmDepthPipeline.GetPipeline() != VK_NULL_HANDLE &&
        m_CsmRenderPass == shadowRenderPass) {
        return true;
    }

    m_CsmDepthPipeline.Cleanup();
    m_CsmRenderPass = VK_NULL_HANDLE;

    const std::array<VkVertexInputBindingDescription, 2> vertexBindings = {
        MakeVertexBinding(0, sizeof(TerrainVertex), VK_VERTEX_INPUT_RATE_VERTEX),
        MakeVertexBinding(1, sizeof(TerrainChunkInstance), VK_VERTEX_INPUT_RATE_INSTANCE)
    };
    const std::array<VkVertexInputAttributeDescription, 4> vertexAttributes = {
        MakeVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, position)),
        MakeVertexAttribute(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, originSize)),
        MakeVertexAttribute(2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, uvRect)),
        MakeVertexAttribute(3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainChunkInstance, params))
    };

    PipelineConfig config;
    config.vertShader = "terrain_csm_depth.vert.spv";
    config.fragShader = "terrain_depth.frag.spv";
    config.topology = m_PrimitiveRestartSupported
        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.primitiveRestartEnable = m_PrimitiveRestartSupported;
    config.cullMode = VK_CULL_MODE_NONE;
    config.depthTest = true;
    config.depthWrite = true;
    config.depthCompareOp = VK_COMPARE_OP_LESS;
    config.colorAttachmentCount = 0;
    config.subpass = 0;
    config.vertexBindings.assign(vertexBindings.begin(), vertexBindings.end());
    config.vertexAttributes.assign(vertexAttributes.begin(), vertexAttributes.end());
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(glm::mat4);
    config.depthBiasEnable = true;
    config.depthBiasConstantFactor = 2.0f;
    config.depthBiasClamp = 0.0f;
    config.depthBiasSlopeFactor = 2.0f;

    if (!m_CsmDepthPipeline.Create(shadowRenderPass, m_DescriptorLayout, config)) {
        return false;
    }
    m_CsmRenderPass = shadowRenderPass;
    return true;
}

bool TerrainRenderer::EnsureGrassDepthPipeline(VkRenderPass shadowRenderPass) {
    if (shadowRenderPass == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE) {
        return false;
    }
    if (m_GrassDepthPipeline.GetPipeline() != VK_NULL_HANDLE &&
        m_GrassCsmRenderPass == shadowRenderPass) {
        return true;
    }

    m_GrassDepthPipeline.Cleanup();
    m_GrassCsmRenderPass = VK_NULL_HANDLE;

    // 顶点阶段 = 主 pass 同一份 grass.vert（零顶点缓冲，几何/风摆/LOD 全同源），
    // 片元为空，只写深度。实例输入布局与主 pass 草管线一致。
    const std::array<VkVertexInputBindingDescription, 1> grassBindings = {
        MakeVertexBinding(0, sizeof(GrassBladeInstance), VK_VERTEX_INPUT_RATE_INSTANCE)
    };
    const std::array<VkVertexInputAttributeDescription, 2> grassAttributes = {
        MakeVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GrassBladeInstance, posParams)),
        MakeVertexAttribute(1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GrassBladeInstance, shapeParams))
    };

    PipelineConfig config;
    config.vertShader = "grass.vert.spv";
    config.fragShader = "grass_depth.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    config.primitiveRestartEnable = false;
    config.cullMode = VK_CULL_MODE_NONE;
    config.depthTest = true;
    config.depthWrite = true;
    config.depthCompareOp = VK_COMPARE_OP_LESS;
    config.colorAttachmentCount = 0;
    config.subpass = 0;
    config.vertexBindings.assign(grassBindings.begin(), grassBindings.end());
    config.vertexAttributes.assign(grassAttributes.begin(), grassAttributes.end());
    config.depthBiasEnable = true;
    config.depthBiasConstantFactor = 2.0f;
    config.depthBiasClamp = 0.0f;
    config.depthBiasSlopeFactor = 2.0f;
    // 级联矩阵走 push constant（见主 pass 草管线处的注释）。
    config.usePushConstants = true;
    config.pushConstantRange = {VK_SHADER_STAGE_VERTEX_BIT, 0,
                                sizeof(glm::mat4) + sizeof(glm::vec4)};

    if (!m_GrassDepthPipeline.Create(shadowRenderPass, m_DescriptorLayout, config)) {
        // 草影管线失败只降级为"草不投影"，不影响地形阴影。
        LOGE("[TerrainRenderer] grass depth pipeline creation failed - grass shadows disabled");
        return false;
    }
    m_GrassCsmRenderPass = shadowRenderPass;
    return true;
}

bool TerrainRenderer::BuildPatch(TerrainPatch& patch, uint32_t resolution) {
    patch.Cleanup();
    if (resolution < 2) {
        return false;
    }

    const size_t vertexCount = static_cast<size_t>(resolution) * resolution;
    std::vector<TerrainVertex> vertices(vertexCount);
    std::vector<uint32_t> indices;
    constexpr uint32_t kPrimitiveRestartIndex = std::numeric_limits<uint32_t>::max();
    if (m_PrimitiveRestartSupported) {
        const size_t interiorStripIndices = static_cast<size_t>(resolution - 1) *
                                            static_cast<size_t>(resolution) * 2u +
                                            static_cast<size_t>(resolution - 2);
        indices.reserve(interiorStripIndices);
    } else {
        const size_t quadCount = static_cast<size_t>(resolution - 1) * (resolution - 1);
        indices.reserve(quadCount * 6u);
    }

    for (uint32_t z = 0; z < resolution; ++z) {
        for (uint32_t x = 0; x < resolution; ++x) {
            const size_t index = static_cast<size_t>(z) * resolution + x;
            const glm::vec2 uv(static_cast<float>(x) / static_cast<float>(resolution - 1),
                               static_cast<float>(z) / static_cast<float>(resolution - 1));
            vertices[index].position = uv;
        }
    }

    if (m_PrimitiveRestartSupported) {
        // 每一行是一个独立 triangle strip，行间用 primitive restart 分隔。
        // 顺序 [top-left, bottom-left, top-right, bottom-right] 与原三角形
        // 列表保持相同的朝向，且避免为换行添加退化三角形。
        for (uint32_t z = 0; z + 1 < resolution; ++z) {
            if (z > 0) {
                indices.push_back(kPrimitiveRestartIndex);
            }
            for (uint32_t x = 0; x < resolution; ++x) {
                indices.push_back(z * resolution + x);
                indices.push_back((z + 1) * resolution + x);
            }
        }
    } else {
        // 极少数不支持 primitiveRestart 的设备回退到 triangle list，
        // 避免 UINT32_MAX 被当成普通顶点索引。
        for (uint32_t z = 0; z + 1 < resolution; ++z) {
            for (uint32_t x = 0; x + 1 < resolution; ++x) {
                const uint32_t a = z * resolution + x;
                const uint32_t b = a + 1;
                const uint32_t d = a + resolution;
                const uint32_t c = d + 1;
                indices.push_back(a);
                indices.push_back(c);
                indices.push_back(b);
                indices.push_back(a);
                indices.push_back(d);
                indices.push_back(c);
            }
        }
    }

    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!patch.vertexBuffer.Create(static_cast<VkDeviceSize>(vertices.size() * sizeof(TerrainVertex)),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
        return false;
    }
    patch.vertexBuffer.Write(vertices.data(), static_cast<VkDeviceSize>(vertices.size() * sizeof(TerrainVertex)));

    if (!patch.indexBuffer.Create(static_cast<VkDeviceSize>(indices.size() * sizeof(uint32_t)),
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT, memory)) {
        patch.Cleanup();
        return false;
    }
    patch.indexBuffer.Write(indices.data(), static_cast<VkDeviceSize>(indices.size() * sizeof(uint32_t)));
    patch.indexCount = static_cast<uint32_t>(indices.size());
    patch.resolution = resolution;
    return true;
}

bool TerrainRenderer::CreateInstanceBuffers(Resource& resource, size_t capacity, bool probeView) {
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    size_t& targetCapacity =
        probeView ? resource.probeInstanceCapacity : resource.instanceCapacity;
    targetCapacity = std::max<size_t>(1, capacity);

    auto createBufferSet = [&](auto& buffers) {
        for (auto& buffer : buffers) {
            buffer.Cleanup();
        }
        for (auto& buffer : buffers) {
            if (!buffer.Create(static_cast<VkDeviceSize>(targetCapacity * sizeof(TerrainChunkInstance)),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
                for (auto& cleanup : buffers) {
                    cleanup.Cleanup();
                }
                return false;
            }
        }
        return true;
    };

    if (!probeView) {
        if (!createBufferSet(resource.instanceBuffers)) {
            targetCapacity = 0;
            return false;
        }
        return true;
    }

    // The six probe faces are recorded back-to-back.  Give every face its own
    // host-written instance stream, just like the per-face camera UBOs.
    for (auto& frameBuffers : resource.probeInstanceBuffers) {
        if (!createBufferSet(frameBuffers)) {
            for (auto& cleanupFrame : resource.probeInstanceBuffers) {
                for (auto& cleanup : cleanupFrame) {
                    cleanup.Cleanup();
                }
            }
            targetCapacity = 0;
            return false;
        }
    }
    return true;
}

bool TerrainRenderer::CreateUniformBuffers(Resource& resource) {
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        resource.uniformBuffers[frame] = std::make_unique<VulkanBuffer>();
        if (!resource.uniformBuffers[frame]->Create(sizeof(TerrainUniformData),
                                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                    memory)) {
            for (auto& cleanup : resource.uniformBuffers) {
                cleanup.reset();
            }
            return false;
        }
        for (auto* viewUbos : {&resource.gameUniformBuffers, &resource.shadowUniformBuffers}) {
            auto& uniform = (*viewUbos)[frame];
            uniform = std::make_unique<VulkanBuffer>();
            if (!uniform->Create(sizeof(TerrainUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory)) {
                return false;
            }
        }
    }
    // 探针 [frame][face] 相机 UBO。6 面在同一命令缓冲里逐面录制，host memcpy 写的
    // UBO 没有命令流排序，只有按面分段才能让每面拿到自己的矩阵。
    // 分配失败不致命：UpdateUniform / ViewDescriptorSet 都会判空并回退主集。
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        for (int face = 0; face < kProbeFaceCount; ++face) {
            auto& probeUbo = resource.probeUniformBuffers[frame][static_cast<size_t>(face)];
            probeUbo = std::make_unique<VulkanBuffer>();
            if (!probeUbo->Create(sizeof(TerrainUniformData),
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  memory)) {
                LOGW("[TerrainRenderer] probe uniform buffer [frame=%u face=%d] create "
                     "failed; probe terrain will fall back to the main-view UBO",
                     frame, face);
                probeUbo.reset();
            }
        }
    }
    return true;
}

bool TerrainRenderer::CreateDescriptorSets(Resource& resource) {
    if (g_Device == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE ||
        m_DescriptorPool == VK_NULL_HANDLE || !g_TexturePool) {
        return false;
    }

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(m_DescriptorLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, kFramesInFlight> allocatedSets{};
    if (vkAllocateDescriptorSets(g_Device, &allocInfo, allocatedSets.data()) != VK_SUCCESS) {
        resource.descriptorSets.fill(VK_NULL_HANDLE);
        return false;
    }
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        resource.descriptorSets[frame] = allocatedSets[frame];
    }
    for (auto* viewSets : {&resource.gameDescriptorSets, &resource.shadowDescriptorSets}) {
        if (vkAllocateDescriptorSets(g_Device, &allocInfo, viewSets->data()) != VK_SUCCESS) {
            viewSets->fill(VK_NULL_HANDLE);
            return false;
        }
    }

    // 探针面集：一次分配 [frame][face] 共 kFramesInFlight * kProbeFaceCount 套，
    // 与主集共用同一 layout/pool，只有 dstBinding 0 指向各自的相机 UBO。
    // 分配失败不致命：ViewDescriptorSet 会回退主集（6 面退化为共用主相机矩阵）。
    {
        constexpr size_t kProbeSetCount =
            static_cast<size_t>(kFramesInFlight) * static_cast<size_t>(kProbeFaceCount);
        std::array<VkDescriptorSet, kProbeSetCount> probeAlloc{};
        std::array<VkDescriptorSetLayout, kProbeSetCount> probeLayouts{};
        probeLayouts.fill(m_DescriptorLayout);
        allocInfo.descriptorSetCount = static_cast<uint32_t>(probeLayouts.size());
        allocInfo.pSetLayouts = probeLayouts.data();
        const bool probeOk =
            vkAllocateDescriptorSets(g_Device, &allocInfo, probeAlloc.data()) == VK_SUCCESS;
        if (!probeOk) {
            LOGW("[TerrainRenderer] probe descriptor set allocation failed; probe terrain "
                 "will bind the main-view descriptor set");
        }
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
            for (int face = 0; face < kProbeFaceCount; ++face) {
                const size_t flat = static_cast<size_t>(frame) *
                                        static_cast<size_t>(kProbeFaceCount) +
                                    static_cast<size_t>(face);
                resource.probeDescriptorSets[frame][static_cast<size_t>(face)] =
                    probeOk ? probeAlloc[flat] : VK_NULL_HANDLE;
            }
        }
    }

    auto getImage = [](const std::string& name, VkDescriptorImageInfo& image) -> bool {
        const TextureInfo* texture = g_TexturePool ? g_TexturePool->GetTexture(name) : nullptr;
        if (!texture || texture->imageView == VK_NULL_HANDLE) {
            return false;
        }
        image.imageView = texture->imageView;
        image.sampler = g_TexturePool->GetSampler(name);
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return image.sampler != VK_NULL_HANDLE;
    };

    // 同一套 7 张图的写入复用到主集与 6 个探针面集上，只有 UBO 不同。
    auto writeSet = [](VkDescriptorSet descriptorSet, VkBuffer uniformBuffer,
                       const std::array<VkDescriptorImageInfo, 7>& images) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(TerrainUniformData);

        std::array<VkWriteDescriptorSet, 8> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        for (uint32_t binding = 1; binding < writes.size(); ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = descriptorSet;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &images[binding - 1];
        }
        vkUpdateDescriptorSets(g_Device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    };

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        std::array<VkDescriptorImageInfo, 7> images{};
        if (!getImage(resource.heightmapKey, images[0])) {
            return false;
        }
        for (int layer = 0; layer < 4; ++layer) {
            if (!getImage(resource.layerKeys[static_cast<size_t>(layer)], images[static_cast<size_t>(layer + 1)])) {
                return false;
            }
        }
        if (!getImage(resource.controlKey, images[5])) {
            return false;
        }
        if (!getImage(resource.waterKey, images[6])) {
            return false;
        }

        if (resource.uniformBuffers[frame]) {
            writeSet(resource.descriptorSets[frame],
                     resource.uniformBuffers[frame]->GetBuffer(), images);
        }
        writeSet(resource.gameDescriptorSets[frame], resource.gameUniformBuffers[frame]->GetBuffer(), images);
        writeSet(resource.shadowDescriptorSets[frame], resource.shadowUniformBuffers[frame]->GetBuffer(), images);
        for (int face = 0; face < kProbeFaceCount; ++face) {
            const VkDescriptorSet probeSet =
                resource.probeDescriptorSets[frame][static_cast<size_t>(face)];
            const VulkanBuffer* probeUbo =
                resource.probeUniformBuffers[frame][static_cast<size_t>(face)].get();
            if (probeSet == VK_NULL_HANDLE || probeUbo == nullptr) {
                continue;
            }
            writeSet(probeSet, probeUbo->GetBuffer(), images);
        }
    }
    return true;
}

bool TerrainRenderer::EnsureInstanceCapacity(Resource& resource, size_t visibleCount,
                                             bool probeView) {
    const size_t currentCapacity =
        probeView ? resource.probeInstanceCapacity : resource.instanceCapacity;
    if (visibleCount <= currentCapacity) {
        return true;
    }
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    const size_t newCapacity = std::max(visibleCount, std::max<size_t>(1, currentCapacity * 2));
    return CreateInstanceBuffers(resource, newCapacity, probeView);
}

bool TerrainRenderer::EnsureCsmInstanceCapacity(Resource& resource, size_t visibleCount) {
    if (visibleCount <= resource.csmInstanceCapacity) {
        return true;
    }
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }

    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const size_t newCapacity = std::max(
        visibleCount, std::max<size_t>(1, resource.csmInstanceCapacity * 2));
    for (auto& buffer : resource.csmInstanceBuffers) {
        buffer.Cleanup();
        if (!buffer.Create(static_cast<VkDeviceSize>(newCapacity * sizeof(TerrainChunkInstance)),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
            for (auto& cleanup : resource.csmInstanceBuffers) {
                cleanup.Cleanup();
            }
            resource.csmInstanceCapacity = 0;
            return false;
        }
    }
    resource.csmInstanceCapacity = newCapacity;
    return true;
}

void TerrainRenderer::UpdateUniform(Resource& resource,
                                    const glm::mat4& projView,
                                    const glm::mat4& prevProjView,
                                    const glm::vec3& cameraPosition,
                                    bool applyTAAJitter,
                                    int viewSlot,
                                    int probeFace) {
    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    TerrainUniformData uniform;
    uniform.projView = projView;
    uniform.prevProjView = prevProjView;
    uniform.model = resource.model;
    uniform.prevModel = resource.hasPreviousModel ? resource.previousModel : resource.model;
    uniform.normalMatrix = glm::transpose(glm::inverse(resource.model));
    uniform.heightParams = glm::vec4(resource.settings.heightScale,
                                     resource.settings.heightOffset,
                                     std::max(0.001f, resource.settings.materialTiling),
                                     std::max(0.001f, resource.settings.worldSize.x));
    uniform.materialParams = glm::vec4(std::max(0.001f, resource.settings.worldSize.y),
                                       resource.useControlMap ? 1.0f : 0.0f,
                                       std::max(0.01f, resource.settings.blendSharpness),
                                       kTerrainWaterMaxDepth);
    uniform.cameraPosition = glm::vec4(cameraPosition, 1.0f);
    uniform.taaJitter = applyTAAJitter
        ? glm::vec4(g_CurrentTAAJitter, 0.0f, 0.0f)
        : glm::vec4(0.0f);
    uniform.timeWind = glm::vec4(static_cast<float>(SDL_GetTicks()) * 0.001f,
                                 1.0f, kGrassViewDistance, 1.0f);
    // 探针 6 面共用 viewSlot=2，但按 probeFace 各自持有一份 UBO：host memcpy 写的
    // UBO 没有命令流排序，6 面录完内存里只剩最后一面的矩阵（见 kProbeFaceCount）。
    if (IsProbeViewSlot(viewSlot)) {
        const size_t face = static_cast<size_t>(
            std::clamp(probeFace, 0, kProbeFaceCount - 1));
        VulkanBuffer* probeUbo = resource.probeUniformBuffers[frame][face].get();
        if (probeUbo != nullptr) {
            probeUbo->Write(&uniform, sizeof(uniform));
        }
        return;
    }
    VulkanBuffer* target = !applyTAAJitter ? resource.shadowUniformBuffers[frame].get()
        : viewSlot == 1 ? resource.gameUniformBuffers[frame].get()
                        : resource.uniformBuffers[frame].get();
    if (target != nullptr) target->Write(&uniform, sizeof(uniform));
}

VkDescriptorSet TerrainRenderer::ViewDescriptorSet(const Resource& resource, uint32_t frame,
                                                   int viewSlot, int probeFace) const {
    if (frame < kFramesInFlight && IsProbeViewSlot(viewSlot)) {
        const size_t face = static_cast<size_t>(
            std::clamp(probeFace, 0, kProbeFaceCount - 1));
        const VkDescriptorSet probeSet = resource.probeDescriptorSets[frame][face];
        if (probeSet != VK_NULL_HANDLE) {
            return probeSet;
        }
    }
    if (frame >= kFramesInFlight) return VK_NULL_HANDLE;
    return viewSlot == 1 ? resource.gameDescriptorSets[frame] : resource.descriptorSets[frame];
}

void TerrainRenderer::Render(VkCommandBuffer commandBuffer, int width, int height,
                             const glm::mat4& projView,
                             const glm::mat4& prevProjView,
                             const glm::vec3& cameraPosition,
                             int viewSlot,
                             int probeFace) {
    RenderInternal(commandBuffer, width, height, projView, prevProjView, cameraPosition,
                   false, viewSlot, probeFace);
}

void TerrainRenderer::RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                                         const glm::mat4& projView,
                                         const glm::vec3& cameraPosition,
                                         int viewSlot,
                                         int probeFace) {
    RenderInternal(commandBuffer, width, height, projView, projView, cameraPosition,
                   true, viewSlot, probeFace);
}

void TerrainRenderer::RenderCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& shadowProjView,
                                     const glm::vec3& cameraPosition) {
    if (commandBuffer == VK_NULL_HANDLE || width <= 0 || height <= 0 ||
        m_PreparedResources.empty() || m_CsmDepthPipeline.GetPipeline() == VK_NULL_HANDLE) {
        return;
    }

    const VkPipeline pipeline = m_CsmDepthPipeline.GetPipeline();
    const VkPipelineLayout pipelineLayout = m_CsmDepthPipeline.GetLayout();
    if (pipelineLayout == VK_NULL_HANDLE) {
        return;
    }

    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(glm::mat4), &shadowProjView);

    for (Resource* resource : m_PreparedResources) {
        if (!resource) {
            continue;
        }
        const size_t visibleCount = resource->chunks.GetVisibleCount();
        if (visibleCount == 0 || !EnsureCsmInstanceCapacity(*resource, visibleCount)) {
            continue;
        }

        std::vector<TerrainChunkInstance> instances;
        instances.reserve(visibleCount);
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            const float edgeIntervals = static_cast<float>(
                std::max(resource->patches[static_cast<size_t>(lod)].resolution, 2u) - 1u);
            for (TerrainChunkInstance instance : visible) {
                instance.params.z = edgeIntervals;
                instances.push_back(instance);
            }
        }
        resource->csmInstanceBuffers[frame].Write(
            instances.data(), static_cast<VkDeviceSize>(instances.size() * sizeof(TerrainChunkInstance)));
        // The CSM vertex shader takes the cascade matrix via push constants;
        // the UBO still supplies the terrain model and heightmap parameters.
        UpdateUniform(*resource, shadowProjView, shadowProjView, cameraPosition, false);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &resource->shadowDescriptorSets[frame], 0, nullptr);

        uint32_t firstInstance = 0;
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            if (visible.empty()) {
                continue;
            }

            VkBuffer vertexBuffers[2] = {
                resource->patches[static_cast<size_t>(lod)].vertexBuffer.GetBuffer(),
                resource->csmInstanceBuffers[frame].GetBuffer()
            };
            VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer,
                                 resource->patches[static_cast<size_t>(lod)].indexBuffer.GetBuffer(),
                                 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(commandBuffer,
                             resource->patches[static_cast<size_t>(lod)].indexCount,
                             static_cast<uint32_t>(visible.size()),
                             0, 0, firstInstance);
            firstInstance += static_cast<uint32_t>(visible.size());
        }
    }
}

bool TerrainRenderer::BuildTerrainMdiTiles(
    Resource& resource, const glm::vec3& cameraPosition,
    const std::array<Plane, 6>& frustumPlanes, int viewSlot) {
    // The candidate vectors are shared by the view-slot uploads.  viewSlot only
    // selects the destination buffer segment in RecordTerrainGpuCull; it is not
    // part of the CPU culling result key.
    (void)viewSlot;
    const uint32_t chunkCount = static_cast<uint32_t>(
        std::clamp(resource.settings.chunkCount, 1, 256));
    const uint32_t gridCount = chunkCount * kTerrainMdiTileSubdivision;
    const uint64_t tileCount64 = static_cast<uint64_t>(gridCount) * gridCount;
    if (gridCount == 0 || tileCount64 > kTerrainMdiMaxTiles) {
        resource.terrainMdiGridCount = 0;
        resource.terrainMdiTileCount = 0;
        resource.terrainMdiCullCacheValid = false;
        return false;
    }

    const uint32_t tileCount = static_cast<uint32_t>(tileCount64);
    const glm::vec2 worldSize = glm::max(resource.settings.worldSize, glm::vec2(1.0f));
    if (resource.terrainMdiLocalBounds.size() != tileCount) {
        resource.terrainMdiLocalBounds.resize(tileCount);
        resource.terrainMdiBoundsDirty = true;
        resource.terrainMdiCullCacheValid = false;
    }

    // Tile bounds only change after a heightmap edit or a resource rebuild.  Keep
    // this scan out of the per-view hot path; model transforms are applied below.
    if (resource.terrainMdiBoundsDirty) {
        const float height0 = resource.settings.heightOffset;
        const float height1 = resource.settings.heightOffset + resource.settings.heightScale;
        const float defaultMinY = std::min(height0, height1) - 0.5f;
        const float defaultMaxY = std::max(height0, height1) + 0.5f;
        const bool hasHeightMirror = !resource.heightmapCpu.empty() &&
                                     resource.heightmapWidth >= 2 &&
                                     resource.heightmapHeight >= 2;

        for (uint32_t z = 0; z < gridCount; ++z) {
            const float z0 = -worldSize.y * 0.5f + worldSize.y *
                             (static_cast<float>(z) / static_cast<float>(gridCount));
            const float z1 = -worldSize.y * 0.5f + worldSize.y *
                             (static_cast<float>(z + 1u) / static_cast<float>(gridCount));
            for (uint32_t x = 0; x < gridCount; ++x) {
                const float x0 = -worldSize.x * 0.5f + worldSize.x *
                                 (static_cast<float>(x) / static_cast<float>(gridCount));
                const float x1 = -worldSize.x * 0.5f + worldSize.x *
                                 (static_cast<float>(x + 1u) / static_cast<float>(gridCount));

                float minY = defaultMinY;
                float maxY = defaultMaxY;
                if (hasHeightMirror) {
                    const uint32_t width = resource.heightmapWidth;
                    const uint32_t height = resource.heightmapHeight;
                    const uint32_t maxSampleX = width - 1u;
                    const uint32_t maxSampleY = height - 1u;
                    const uint32_t tileSampleX0 = (x * maxSampleX) / gridCount;
                    const uint32_t tileSampleX1 = ((x + 1u) * maxSampleX) / gridCount;
                    const uint32_t localSampleY0 = static_cast<uint32_t>(
                        (static_cast<uint64_t>(z) * maxSampleY) / gridCount);
                    const uint32_t localSampleY1 = static_cast<uint32_t>(
                        (static_cast<uint64_t>(z + 1u) * maxSampleY) / gridCount);
                    // The CPU mirror keeps image rows in top-left order, while
                    // the uploaded height texture is bottom-up and the vertex
                    // shader samples local z/v=0 from the bottom image row.
                    // Reverse the interval so the tile AABB follows the actual
                    // heightfield rendered by the shader rather than the
                    // opposite heightmap band.
                    const uint32_t tileSampleY0 = maxSampleY - localSampleY1;
                    const uint32_t tileSampleY1 = maxSampleY - localSampleY0;
                    const uint32_t sampleX0 = tileSampleX0 > 0 ? tileSampleX0 - 1u : 0u;
                    const uint32_t sampleY0 = tileSampleY0 > 0 ? tileSampleY0 - 1u : 0u;
                    const uint32_t sampleX1 = std::min(maxSampleX, tileSampleX1 + 1u);
                    const uint32_t sampleY1 = std::min(maxSampleY, tileSampleY1 + 1u);

                    uint16_t minSample = 0xffffu;
                    uint16_t maxSample = 0u;
                    for (uint32_t sampleY = sampleY0; sampleY <= sampleY1; ++sampleY) {
                        const uint16_t* row = &resource.heightmapCpu[
                            static_cast<size_t>(sampleY) * width];
                        for (uint32_t sampleX = sampleX0; sampleX <= sampleX1; ++sampleX) {
                            minSample = std::min(minSample, row[sampleX]);
                            maxSample = std::max(maxSample, row[sampleX]);
                        }
                    }
                    const float sampleScale = resource.settings.heightScale /
                                              65535.0f;
                    const float sampledY0 = resource.settings.heightOffset +
                                            static_cast<float>(minSample) * sampleScale;
                    const float sampledY1 = resource.settings.heightOffset +
                                            static_cast<float>(maxSample) * sampleScale;
                    minY = std::min(sampledY0, sampledY1) - 0.5f;
                    maxY = std::max(sampledY0, sampledY1) + 0.5f;
                }

                resource.terrainMdiLocalBounds[static_cast<size_t>(z) * gridCount + x] =
                    AABB(glm::vec3(x0, minY, z0), glm::vec3(x1, maxY, z1));
            }
        }
        resource.terrainMdiBoundsDirty = false;
        resource.terrainMdiCullCacheValid = false;
    }

    // Reuse the complete CPU coarse-cull/LOD result when the culling reference
    // is unchanged.  GPU tile testing and indirect-command generation still run
    // for the current frame, so this never reuses a stale command buffer.
    const bool cacheHit = resource.terrainMdiCullCacheValid &&
                          SameVec3(resource.terrainMdiCachedCameraPosition,
                                   cameraPosition) &&
                          SameMat4(resource.terrainMdiCachedModel, resource.model) &&
                          SameFrustum(resource.terrainMdiCachedFrustumPlanes,
                                      frustumPlanes);
    if (cacheHit) {
        resource.terrainMdiGridCount = gridCount;
        return true;
    }

    resource.terrainMdiGridCount = gridCount;
    resource.terrainMdiTileCount = 0;
    resource.terrainMdiCandidateLodCounts.fill(0);
    resource.terrainMdiTiles.clear();
    resource.terrainMdiInstances.clear();
    resource.terrainMdiTiles.reserve(tileCount);
    resource.terrainMdiInstances.reserve(tileCount);

    const glm::vec3 cullMargin(kTerrainMdiCullSafetyMargin);
    std::vector<int8_t> lodGrid(tileCount, static_cast<int8_t>(-1));
    for (uint32_t z = 0; z < gridCount; ++z) {
        for (uint32_t x = 0; x < gridCount; ++x) {
            const size_t tileIndex = static_cast<size_t>(z) * gridCount + x;
            const AABB worldBounds = resource.terrainMdiLocalBounds[tileIndex].Transform(resource.model);
            const AABB cullBounds(worldBounds.min - cullMargin,
                                  worldBounds.max + cullMargin);
            // 第一级：CPU 粗筛。与草叶级流程一样，先用 tile AABB 做视锥
            // 和水平距离门控，只把可能进入当前视图的 tile 压缩进 GPU 流。
            // GPU compute 仍会对这份候选流重复做精确测试，避免 CPU 浮点边界
            // 或视图缓存滞后导致错误绘制。
            if (!cullBounds.IsInsideFrustum(frustumPlanes)) {
                continue;
            }
            const glm::vec2 cameraXZ(cameraPosition.x, cameraPosition.z);
            const glm::vec2 closestXZ = glm::clamp(
                cameraXZ,
                glm::vec2(cullBounds.min.x, cullBounds.min.z),
                glm::vec2(cullBounds.max.x, cullBounds.max.z));
            if (resource.settings.viewDistance > 0.0f &&
                glm::distance(cameraXZ, closestXZ) > resource.settings.viewDistance) {
                continue;
            }
            const float distance = glm::distance(cameraPosition, worldBounds.GetCenter());

            int lod = 0;
            const int maxLod = std::clamp(resource.settings.maxLod, 0, 2);
            if (maxLod >= 1 && distance >= resource.settings.lod0Distance) {
                lod = 1;
            }
            if (maxLod >= 2 && distance >= resource.settings.lod1Distance) {
                lod = 2;
            }
            lodGrid[tileIndex] = static_cast<int8_t>(std::clamp(lod, 0, maxLod));
        }
    }

    auto coarserDelta = [&](int x, int z, int lod) -> uint32_t {
        if (x < 0 || z < 0 || x >= static_cast<int>(gridCount) ||
            z >= static_cast<int>(gridCount)) {
            return 0;
        }
        const int neighborLod = lodGrid[static_cast<size_t>(z) * gridCount +
                                        static_cast<size_t>(x)];
        if (neighborLod < 0) {
            return 0;
        }
        return static_cast<uint32_t>(std::clamp(neighborLod - lod, 0, 3));
    };

    for (uint32_t z = 0; z < gridCount; ++z) {
        for (uint32_t x = 0; x < gridCount; ++x) {
            const size_t tileIndex = static_cast<size_t>(z) * gridCount + x;
            const AABB& localBounds = resource.terrainMdiLocalBounds[tileIndex];
            const AABB worldBounds = localBounds.Transform(resource.model);
            const AABB cullBounds(worldBounds.min - cullMargin,
                                  worldBounds.max + cullMargin);
            const int lod = lodGrid[tileIndex];

            if (lod < 0) {
                continue;
            }
            const uint32_t commandIndex =
                resource.terrainMdiCandidateLodCounts[static_cast<size_t>(lod)]++;

            TerrainChunkInstance instance;
            instance.originSize = glm::vec4(localBounds.min.x, localBounds.min.z,
                                             localBounds.max.x - localBounds.min.x,
                                             localBounds.max.z - localBounds.min.z);
            instance.uvRect = glm::vec4(static_cast<float>(x), static_cast<float>(z),
                                        static_cast<float>(gridCount), 0.0f);

            uint32_t edgeLodDeltas = 0;
            edgeLodDeltas =
                coarserDelta(static_cast<int>(x), static_cast<int>(z) - 1, lod) |
                (coarserDelta(static_cast<int>(x) + 1, static_cast<int>(z), lod) << 2u) |
                (coarserDelta(static_cast<int>(x), static_cast<int>(z) + 1, lod) << 4u) |
                (coarserDelta(static_cast<int>(x) - 1, static_cast<int>(z), lod) << 6u);
            const float edgeIntervals = static_cast<float>(
                std::max(resource.terrainMdiPatches[static_cast<size_t>(lod)].resolution,
                         2u) - 1u);
            instance.params = glm::vec4(static_cast<float>(lod),
                                        static_cast<float>(edgeLodDeltas),
                                        edgeIntervals, 0.0f);
            resource.terrainMdiInstances.push_back(instance);

            TerrainMdiTile tile;
            tile.minBounds = glm::vec4(cullBounds.min, 0.0f);
            tile.maxBounds = glm::vec4(cullBounds.max, 0.0f);
            tile.params = glm::uvec4(
                static_cast<uint32_t>(lod), commandIndex,
                resource.terrainMdiPatches[static_cast<size_t>(lod)].indexCount, 0u);
            resource.terrainMdiTiles.push_back(tile);
        }
    }
    resource.terrainMdiTileCount = static_cast<uint32_t>(resource.terrainMdiTiles.size());
    resource.terrainMdiCachedCameraPosition = cameraPosition;
    resource.terrainMdiCachedModel = resource.model;
    resource.terrainMdiCachedFrustumPlanes = frustumPlanes;
    resource.terrainMdiCullCacheValid = true;
    ++resource.terrainMdiCandidateRevision;
    return true;
}

bool TerrainRenderer::EnsureTerrainMdiCullPipeline() {
    if (m_TerrainMdiCullPipeline != VK_NULL_HANDLE) {
        return true;
    }
    if (m_TerrainMdiCullDisabled || g_Device == VK_NULL_HANDLE ||
        g_PhysicalDevice == VK_NULL_HANDLE) {
        return false;
    }
    static const bool disabledByEnv = [] {
        const char* env = std::getenv("MIKAN_TERRAIN_MDI");
        return env != nullptr && std::strcmp(env, "0") == 0;
    }();
    if (disabledByEnv) {
        m_TerrainMdiCullDisabled = true;
        LOGI("[TerrainRenderer] terrain MDI disabled by MIKAN_TERRAIN_MDI=0; using CPU fallback");
        return false;
    }

    if (!m_TerrainMdiSupported) {
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &features);
        if (!features.multiDrawIndirect || !features.drawIndirectFirstInstance) {
            m_TerrainMdiCullDisabled = true;
            LOGW("[TerrainRenderer] terrain MDI unavailable: multiDrawIndirect=%d "
                 "drawIndirectFirstInstance=%d; using CPU fallback",
                 features.multiDrawIndirect ? 1 : 0,
                 features.drawIndirectFirstInstance ? 1 : 0);
            return false;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(g_PhysicalDevice, &properties);
        m_TerrainMdiMaxDrawCount = properties.limits.maxDrawIndirectCount;
        if (m_TerrainMdiMaxDrawCount == 0) {
            m_TerrainMdiCullDisabled = true;
            LOGW("[TerrainRenderer] terrain MDI unavailable: maxDrawIndirectCount=0");
            return false;
        }
        m_TerrainMdiSupported = true;
    }

    const std::string spvPath = EngineConfig::GetShaderPath("terrain_cull.comp.spv");
    std::vector<char> code;
    if (SDL_IOStream* io = SDL_IOFromFile(spvPath.c_str(), "rb")) {
        const Sint64 size = SDL_GetIOSize(io);
        if (size > 0) {
            code.resize(static_cast<size_t>(size));
            if (SDL_ReadIO(io, code.data(), static_cast<size_t>(size)) !=
                static_cast<size_t>(size)) {
                code.clear();
            }
        }
        SDL_CloseIO(io);
    }
    if (code.empty() || (code.size() % 4) != 0) {
        LOGW("[TerrainRenderer] terrain MDI shader unavailable: %s; using CPU fallback",
             spvPath.c_str());
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = code.size();
    shaderInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    if (vkCreateShaderModule(g_Device, &shaderInfo, g_Allocator, &shaderModule) != VK_SUCCESS) {
        LOGW("[TerrainRenderer] terrain MDI shader module creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[6]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator,
                                    &m_TerrainMdiCullDescriptorLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
        LOGW("[TerrainRenderer] terrain MDI descriptor layout creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(TerrainMdiCullPush);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_TerrainMdiCullDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(g_Device, &pipelineLayoutInfo, g_Allocator,
                               &m_TerrainMdiCullPipelineLayout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(g_Device, m_TerrainMdiCullDescriptorLayout, g_Allocator);
        m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
        vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
        LOGW("[TerrainRenderer] terrain MDI pipeline layout creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_TerrainMdiCullPipelineLayout;
    const VkResult pipelineResult = vkCreateComputePipelines(
        g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_TerrainMdiCullPipeline);
    vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
    if (pipelineResult != VK_SUCCESS) {
        vkDestroyPipelineLayout(g_Device, m_TerrainMdiCullPipelineLayout, g_Allocator);
        vkDestroyDescriptorSetLayout(g_Device, m_TerrainMdiCullDescriptorLayout, g_Allocator);
        m_TerrainMdiCullPipelineLayout = VK_NULL_HANDLE;
        m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
        LOGW("[TerrainRenderer] terrain MDI compute pipeline creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets * 4);
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets);
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets);
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = static_cast<uint32_t>(kInitialDescriptorSets);
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator,
                               &m_TerrainMdiCullDescriptorPool) != VK_SUCCESS) {
        vkDestroyPipeline(g_Device, m_TerrainMdiCullPipeline, g_Allocator);
        vkDestroyPipelineLayout(g_Device, m_TerrainMdiCullPipelineLayout, g_Allocator);
        vkDestroyDescriptorSetLayout(g_Device, m_TerrainMdiCullDescriptorLayout, g_Allocator);
        m_TerrainMdiCullPipeline = VK_NULL_HANDLE;
        m_TerrainMdiCullPipelineLayout = VK_NULL_HANDLE;
        m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
        LOGW("[TerrainRenderer] terrain MDI descriptor pool creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    LOGI("[TerrainRenderer] terrain 4x4-tile MDI culling enabled (maxDrawIndirectCount=%u)",
         m_TerrainMdiMaxDrawCount);
    return true;
/*
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator,
                                    &m_TerrainMdiCullDescriptorLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
        LOGW("[TerrainRenderer] terrain MDI descriptor layout creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(TerrainMdiCullPush);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_TerrainMdiCullDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(g_Device, &pipelineLayoutInfo, g_Allocator,
                               &m_TerrainMdiCullPipelineLayout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(g_Device, m_TerrainMdiCullDescriptorLayout, g_Allocator);
        m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
        vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
        LOGW("[TerrainRenderer] terrain MDI pipeline layout creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_TerrainMdiCullPipelineLayout;
    const VkResult pipelineResult = vkCreateComputePipelines(
        g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_TerrainMdiCullPipeline);
    vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
    if (pipelineResult != VK_SUCCESS) {
        vkDestroyPipelineLayout(g_Device, m_TerrainMdiCullPipelineLayout, g_Allocator);
        vkDestroyDescriptorSetLayout(g_Device, m_TerrainMdiCullDescriptorLayout, g_Allocator);
        m_TerrainMdiCullPipelineLayout = VK_NULL_HANDLE;
        m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
        LOGW("[TerrainRenderer] terrain MDI compute pipeline creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets * 2);
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets);
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets);
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = static_cast<uint32_t>(kInitialDescriptorSets);
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator,
                               &m_TerrainMdiCullDescriptorPool) != VK_SUCCESS) {
        vkDestroyPipeline(g_Device, m_TerrainMdiCullPipeline, g_Allocator);
        vkDestroyPipelineLayout(g_Device, m_TerrainMdiCullPipelineLayout, g_Allocator);
        vkDestroyDescriptorSetLayout(g_Device, m_TerrainMdiCullDescriptorLayout, g_Allocator);
        m_TerrainMdiCullPipeline = VK_NULL_HANDLE;
        m_TerrainMdiCullPipelineLayout = VK_NULL_HANDLE;
        m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
        LOGW("[TerrainRenderer] terrain MDI descriptor pool creation failed; using CPU fallback");
        m_TerrainMdiCullDisabled = true;
        return false;
    }

    LOGI("[TerrainRenderer] terrain 4x4-tile MDI culling enabled (maxDrawIndirectCount=%u)",
         m_TerrainMdiMaxDrawCount);
    return true;
*/
}

bool TerrainRenderer::EnsureTerrainMdiBuffers(Resource& resource, uint32_t frame) {
    const uint32_t tileCount = resource.terrainMdiTileCount;
    if (tileCount == 0 || m_TerrainMdiCullDescriptorPool == VK_NULL_HANDLE ||
        tileCount > m_TerrainMdiMaxDrawCount) {
        return false;
    }

    // Allocate once for the complete source grid, not only the current view's
    // candidate count.  SceneView/GameView are recorded before either draw; if
    // the second view has more CPU survivors, reallocating here would discard
    // the first view's already-uploaded instance and indirect command segments.
    const uint32_t requiredCapacity = std::max(tileCount, resource.terrainMdiGridCount);
    if (resource.terrainMdiTileCapacity < requiredCapacity) {
        if (TerrainDiagEnabled()) {
            LOGI("[TerrainDiag] MDI REALLOC cap=%zu -> %u (grid=%u tileCount=%u) "
                 "— 帧内重建会作废已录制的地形绘制命令",
                 resource.terrainMdiTileCapacity, requiredCapacity,
                 resource.terrainMdiGridCount, tileCount);
        }
        if (g_Device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_Device);
        }
        for (auto& buffer : resource.terrainMdiInstanceBuffers) {
            buffer.Cleanup();
        }
        for (auto& buffer : resource.terrainMdiCompactInstanceBuffers) {
            buffer.Cleanup();
        }
        for (auto& buffer : resource.terrainMdiTileBuffers) {
            buffer.Cleanup();
        }
        for (auto& buffer : resource.terrainMdiIndirectBuffers) {
            buffer.Cleanup();
        }
        for (auto& buffer : resource.terrainMdiCullViewBuffers) {
            buffer.Cleanup();
        }

        const VkDeviceSize instanceBytes =
            static_cast<VkDeviceSize>(kTerrainMdiViewSlots) * requiredCapacity *
            sizeof(TerrainChunkInstance);
        const VkDeviceSize compactInstanceBytes =
            static_cast<VkDeviceSize>(kTerrainMdiViewSlots) * 3u * requiredCapacity *
            sizeof(TerrainChunkInstance);
        const VkDeviceSize tileBytes =
            static_cast<VkDeviceSize>(kTerrainMdiViewSlots) * requiredCapacity *
            sizeof(TerrainMdiTile);
        const VkDeviceSize commandBytes =
            static_cast<VkDeviceSize>(kTerrainMdiViewSlots) * 3u *
            sizeof(VkDrawIndexedIndirectCommand);
        const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const VkMemoryPropertyFlags deviceMemory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        bool created = true;
        for (auto& buffer : resource.terrainMdiInstanceBuffers) {
            created = created && buffer.Create(
                instanceBytes,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                hostMemory);
        }
        for (auto& buffer : resource.terrainMdiCompactInstanceBuffers) {
            created = created && buffer.Create(
                compactInstanceBytes,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                deviceMemory);
        }
        for (auto& buffer : resource.terrainMdiTileBuffers) {
            created = created && buffer.Create(tileBytes,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               deviceMemory);
        }
        for (auto& buffer : resource.terrainMdiIndirectBuffers) {
            created = created && buffer.Create(commandBytes,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                               deviceMemory);
        }
        const VkDeviceSize viewBytes = static_cast<VkDeviceSize>(kTerrainMdiViewSlots) *
                                       sizeof(TerrainMdiCullViewGpu);
        for (auto& buffer : resource.terrainMdiCullViewBuffers) {
            created = created && buffer.Create(viewBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                               hostMemory);
        }
        if (!created) {
            for (auto& buffer : resource.terrainMdiInstanceBuffers) {
                buffer.Cleanup();
            }
            for (auto& buffer : resource.terrainMdiCompactInstanceBuffers) {
                buffer.Cleanup();
            }
            for (auto& buffer : resource.terrainMdiTileBuffers) {
                buffer.Cleanup();
            }
            for (auto& buffer : resource.terrainMdiIndirectBuffers) {
                buffer.Cleanup();
            }
            for (auto& buffer : resource.terrainMdiCullViewBuffers) {
                buffer.Cleanup();
            }
            resource.terrainMdiTileCapacity = 0;
    resource.terrainMdiUploadCaches = {};
            LOGW("[TerrainRenderer] terrain MDI buffers unavailable; using CPU fallback");
            return false;
        }
        resource.terrainMdiTileCapacity = requiredCapacity;
        resource.terrainMdiUploadCaches = {};
    }

    if (resource.terrainMdiCullDescriptorSets[frame] == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_TerrainMdiCullDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_TerrainMdiCullDescriptorLayout;
        if (vkAllocateDescriptorSets(g_Device, &allocInfo,
                                     &resource.terrainMdiCullDescriptorSets[frame]) != VK_SUCCESS) {
            resource.terrainMdiCullDescriptorSets[frame] = VK_NULL_HANDLE;
            LOGW("[TerrainRenderer] terrain MDI descriptor set allocation failed; using CPU fallback");
            return false;
        }
    }

    VkDescriptorBufferInfo bufferInfos[4]{};
    bufferInfos[0].buffer = resource.terrainMdiTileBuffers[frame].GetBuffer();
    bufferInfos[0].range = VK_WHOLE_SIZE;
    bufferInfos[1].buffer = resource.terrainMdiIndirectBuffers[frame].GetBuffer();
    bufferInfos[1].range = VK_WHOLE_SIZE;
    bufferInfos[2].buffer = resource.terrainMdiInstanceBuffers[frame].GetBuffer();
    bufferInfos[2].range = VK_WHOLE_SIZE;
    bufferInfos[3].buffer = resource.terrainMdiCompactInstanceBuffers[frame].GetBuffer();
    bufferInfos[3].range = VK_WHOLE_SIZE;
    VkDescriptorImageInfo hizInfo{};
    VkImageView hizView = VK_NULL_HANDLE;
    if (g_SceneRenderer.IsTerrainHiZCullingEnabled()) {
        hizView = g_SceneRenderer.GetHiZShader().GetHiZTextureViewForCulling();
    }
    if (hizView != VK_NULL_HANDLE) {
        hizInfo.imageView = hizView;
        hizInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        // Keep the descriptor complete before the first Hi-Z copy exists. The
        // per-view UBO disables sampling in that frame, so this depth view is
        // only a safe descriptor fallback, never a Hi-Z source.
        hizInfo.imageView = g_GameRenderTarget.GetDepthImageView();
        hizInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    hizInfo.sampler = g_GameRenderTarget.GetHiZSampler();
    if (hizInfo.imageView == VK_NULL_HANDLE || hizInfo.sampler == VK_NULL_HANDLE) {
        // A descriptor with a null image/sampler is invalid even when the
        // shader-side enabled bit is zero. Let the caller use the original
        // CPU terrain path until GameRT has a complete fallback resource.
        if (resource.terrainMdiCullDescriptorSets[frame] != VK_NULL_HANDLE) {
            VkDescriptorSet staleSet = resource.terrainMdiCullDescriptorSets[frame];
            vkFreeDescriptorSets(g_Device, m_TerrainMdiCullDescriptorPool, 1, &staleSet);
            resource.terrainMdiCullDescriptorSets[frame] = VK_NULL_HANDLE;
        }
        return false;
    }

    VkDescriptorBufferInfo viewInfo{};
    viewInfo.buffer = resource.terrainMdiCullViewBuffers[frame].GetBuffer();
    viewInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[6]{};
    for (uint32_t binding = 0; binding < 4; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = resource.terrainMdiCullDescriptorSets[frame];
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &bufferInfos[binding];
    }
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = resource.terrainMdiCullDescriptorSets[frame];
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo = &hizInfo;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = resource.terrainMdiCullDescriptorSets[frame];
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[5].pBufferInfo = &viewInfo;
    vkUpdateDescriptorSets(g_Device, 6, writes, 0, nullptr);
    return true;
}

int TerrainRenderer::FindTerrainMdiView(const Resource& resource, uint32_t frame,
                                        const glm::mat4& projView) const {
    if (frame >= kFramesInFlight) {
        return -1;
    }
    for (int slot = 0; slot < kTerrainMdiViewSlots; ++slot) {
        const auto& cullView = resource.terrainMdiCullViews[frame][static_cast<size_t>(slot)];
        if (cullView.dispatched && cullView.viewProj == projView) {
            return slot;
        }
    }
    return -1;
}

void TerrainRenderer::RecordTerrainGpuCull(VkCommandBuffer commandBuffer,
                                            const glm::mat4& view,
                                            const glm::mat4& proj,
                                            int viewSlot) {
    static bool s_loggedRecordHook = false;
    if (!s_loggedRecordHook) {
        s_loggedRecordHook = true;
        LOGI("[TerrainRenderer] terrain MDI record hook reached (slot=%d)", viewSlot);
    }
    if (TerrainDiagEnabled()) {
        LOGI("[TerrainDiag] RecordCull slot=%d frame=%u prepared=%zu clearedFrame=%d",
             viewSlot, static_cast<unsigned>(GetCurrentFrameIndex() % kFramesInFlight),
             m_PreparedResources.size(),
             m_PreparedResources.empty()
                 ? -1
                 : static_cast<int>(m_PreparedResources.front()->terrainMdiCullClearedFrame));
    }
    if (commandBuffer == VK_NULL_HANDLE || viewSlot < 0 ||
        viewSlot >= kTerrainMdiViewSlots || !EnsureTerrainMdiCullPipeline()) {
        return;
    }

    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    const glm::mat4 viewProj = proj * view;
    const glm::vec3 fallbackCameraPosition = glm::vec3(glm::inverse(view)[3]);
    const std::array<Plane, 6> fallbackFrustumPlanes =
        AABBUtils::ExtractFrustumPlanes(viewProj);

    // 地形 Hi-Z 当前回退关闭：即使草地继续生成/消费独立的 Hi-Z，地面
    // 仍只使用原有 CPU 粗筛 + GPU 视锥/距离细筛/fallback。

    auto updateBufferInChunks = [&](VkBuffer buffer, VkDeviceSize destinationOffset,
                                    const void* data, VkDeviceSize bytes) {
        const auto* source = static_cast<const uint8_t*>(data);
        VkDeviceSize uploaded = 0;
        while (uploaded < bytes) {
            VkDeviceSize chunk = std::min(kMaxUpdateBufferBytes, bytes - uploaded);
            chunk &= ~VkDeviceSize(3);
            if (chunk == 0) {
                break;
            }
            vkCmdUpdateBuffer(commandBuffer, buffer, destinationOffset + uploaded,
                              chunk, source + uploaded);
            uploaded += chunk;
        }
    };

    for (Resource* resource : m_PreparedResources) {
        if (!resource) {
            continue;
        }
        auto& cullViews = resource->terrainMdiCullViews[frame];
        if (resource->terrainMdiCullClearedFrame != GetCurrentFrameSerial()) {
            for (auto& cullView : cullViews) {
                cullView = Resource::TerrainMdiCullView{};
            }
            resource->terrainMdiTileCounts.fill(0);
            resource->terrainMdiCullClearedFrame = GetCurrentFrameSerial();
        }
        auto& cullView = cullViews[static_cast<size_t>(viewSlot)];

        // MDI 命令最终由 RenderInternal 使用本次 draw view/proj 绘制，但
        // SceneView 的地形可见集必须和草地、CPU chunk 一样服从主相机视锥。
        // Prepare 阶段已经把这套参考系缓存到 resource；优先使用缓存，避免
        // 编辑器相机视图把主相机视锥外的 tile 重新送入 GPU。首帧或没有带
        // 视锥的 Prepare 时才回退到本次 draw view，保证资源刚建立时仍可画。
        //
        // 反射探针（第三个视图）**固定不走缓存**：缓存里只有主视图的参考系，
        // 探针复用它会让背向的几面丢掉地形；而探针自己写进缓存又会污染主视图
        // （见 Prepare 的写守卫）。所以探针用本面 draw view 现场提平面。
        const bool useCachedCull =
            resource->terrainMdiUseFrustumCulling && !IsProbeViewSlot(viewSlot);

        if (cullView.dispatched && cullView.viewProj == viewProj &&
            cullView.usesCachedCull == useCachedCull) {
            // Several render paths can prepare the same main view before the
            // actual terrain draw. The first dispatch already populated this
            // frame/slot's compact instance stream and indirect commands.
            continue;
        }

        const std::array<Plane, 6>& cullPlanes = useCachedCull
            ? resource->terrainMdiFrustumPlanes
            : fallbackFrustumPlanes;
        const glm::vec3& cullCameraPosition = useCachedCull
            ? resource->terrainMdiCameraPosition
            : fallbackCameraPosition;

        if (!BuildTerrainMdiTiles(*resource, cullCameraPosition, cullPlanes,
                                  viewSlot)) {
            continue;
        }

        const uint32_t tileCount = resource->terrainMdiTileCount;
        const size_t viewIndex = static_cast<size_t>(viewSlot);
        resource->terrainMdiTileCounts[viewIndex] = tileCount;
        resource->terrainMdiLodTileCounts[viewIndex] =
            resource->terrainMdiCandidateLodCounts;
        if (tileCount == 0) {
            // CPU 粗筛全灭：仍记录该视图已处理，让 RenderInternal 跳过地形
            // 而不是误回退到整块 CPU chunk 绘制；草仍可按自己的流程绘制。
            cullView.dispatched = true;
            cullView.usesCompactInstances = false;
            cullView.usesCachedCull = useCachedCull;
            cullView.viewProj = viewProj;
            continue;
        }
        if (!EnsureTerrainMdiBuffers(*resource, frame)) {
            resource->terrainMdiTileCounts[static_cast<size_t>(viewSlot)] = 0;
            continue;
        }

        TerrainMdiCullViewGpu viewGpu{};
        viewGpu.hizViewProj = viewProj;
        viewGpu.hizParams = glm::uvec4(
            g_GameRenderTarget.GetWidth(),
            g_GameRenderTarget.GetHeight(),
            0u,
            0u);
        resource->terrainMdiCullViewBuffers[frame].Write(
            &viewGpu, sizeof(viewGpu),
            static_cast<VkDeviceSize>(viewSlot) * sizeof(viewGpu));

        const VkDeviceSize instanceSegmentBytes =
            static_cast<VkDeviceSize>(resource->terrainMdiTileCapacity) *
            sizeof(TerrainChunkInstance);
        const VkDeviceSize instanceOffset =
            static_cast<VkDeviceSize>(viewSlot) * instanceSegmentBytes;

        // 当前引擎明确关闭 terrain Hi-Z。此时 CPU 粗筛已经完成同一套
        // 2D/视锥/距离判定，GPU 再重复做一次不会增加可见性信息，只会增加
        // dispatch 和中间实例流写入成本。保留 MDI，但直接把 CPU 候选按 LOD
        // 作为三条间接绘制命令；Hi-Z 重新启用时仍走下面的 GPU 细筛路径。
        const bool terrainHiZEnabled = g_SceneRenderer.IsTerrainHiZCullingEnabled();
        if (!terrainHiZEnabled) {
            // CPU 候选原始顺序按 tile 网格交错了不同 LOD；三条 draw 必须
            // 看到按 LOD 连续的 instance-rate 顶点流，不能直接拿原始前缀。
            auto& lodCache = resource->terrainMdiLodCaches[viewIndex];
            if (lodCache.sourceRevision != resource->terrainMdiCandidateRevision) {
                const auto& candidates = resource->terrainMdiInstances;
                const bool changed = lodCache.sourceInstances.size() != candidates.size() ||
                    std::memcmp(lodCache.sourceInstances.data(), candidates.data(),
                                candidates.size() * sizeof(TerrainChunkInstance)) != 0;
                if (changed) {
                    lodCache.sourceInstances = candidates;
                    lodCache.lodInstances.clear();
                    lodCache.lodInstances.reserve(tileCount);
                    for (uint32_t lod = 0; lod < 3u; ++lod) {
                        lodCache.offsets[lod] = static_cast<uint32_t>(lodCache.lodInstances.size());
                        for (const auto& candidate : candidates) {
                            if (static_cast<uint32_t>(candidate.params.x) == lod)
                                lodCache.lodInstances.push_back(candidate);
                        }
                    }
                    ++lodCache.instanceRevision;
                }
                // Camera movement can change the cull key without changing the visible stream.
                lodCache.sourceRevision = resource->terrainMdiCandidateRevision;
            }
            resource->terrainMdiLodInstanceOffsets[viewIndex] = lodCache.offsets;
            auto& uploadCache = resource->terrainMdiUploadCaches[frame][viewIndex];
            const bool instancesChanged = uploadCache.instanceRevision != lodCache.instanceRevision;
            if (instancesChanged) {
                resource->terrainMdiInstanceBuffers[frame].Write(
                    lodCache.lodInstances.data(),
                    static_cast<VkDeviceSize>(lodCache.lodInstances.size()) * sizeof(TerrainChunkInstance),
                    instanceOffset);
                uploadCache.instanceRevision = lodCache.instanceRevision;
            }
            const VkDeviceSize commandStride = sizeof(VkDrawIndexedIndirectCommand);
            const VkDeviceSize commandOffset =
                static_cast<VkDeviceSize>(viewSlot) * 3u * commandStride;
            std::array<VkDrawIndexedIndirectCommand, 3> lodCommands{};
            for (uint32_t lod = 0; lod < 3u; ++lod) {
                lodCommands[lod].indexCount = resource->terrainMdiPatches[lod].indexCount;
                lodCommands[lod].instanceCount =
                    resource->terrainMdiLodTileCounts[viewIndex][lod];
                lodCommands[lod].firstIndex = 0;
                lodCommands[lod].vertexOffset = 0;
                lodCommands[lod].firstInstance = 0;
            }
            const bool commandsChanged = !uploadCache.commandsValid ||
                std::memcmp(uploadCache.commands.data(), lodCommands.data(), sizeof(lodCommands)) != 0;
            if (commandsChanged) {
                updateBufferInChunks(resource->terrainMdiIndirectBuffers[frame].GetBuffer(),
                                     commandOffset, lodCommands.data(), sizeof(lodCommands));
                uploadCache.commands = lodCommands;
                uploadCache.commandsValid = true;
            }
            if (instancesChanged || commandsChanged) {
            VkMemoryBarrier cpuMdiBarrier{};
            cpuMdiBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            cpuMdiBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
                                          VK_ACCESS_TRANSFER_WRITE_BIT;
            cpuMdiBarrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                          VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                0, 1, &cpuMdiBarrier, 0, nullptr, 0, nullptr);

            }

            cullView.dispatched = true;
            cullView.usesCompactInstances = false;
            cullView.usesCachedCull = useCachedCull;
            cullView.viewProj = viewProj;
            continue;
        }

        // GPU HiZ execution overwrites this segment; invalidate the CPU upload snapshot.
        resource->terrainMdiUploadCaches[frame][viewIndex] = {};
        resource->terrainMdiLodInstanceOffsets[viewIndex].fill(0u);
        resource->terrainMdiInstanceBuffers[frame].Write(
            resource->terrainMdiInstances.data(),
            static_cast<VkDeviceSize>(tileCount) * sizeof(TerrainChunkInstance),
            instanceOffset);

        const VkDeviceSize tileBytes = static_cast<VkDeviceSize>(tileCount) *
                                       sizeof(TerrainMdiTile);
        const VkDeviceSize tileSegmentBytes =
            static_cast<VkDeviceSize>(resource->terrainMdiTileCapacity) *
            sizeof(TerrainMdiTile);
        const VkDeviceSize tileOffset =
            static_cast<VkDeviceSize>(viewSlot) * tileSegmentBytes;
        updateBufferInChunks(resource->terrainMdiTileBuffers[frame].GetBuffer(), tileOffset,
                             resource->terrainMdiTiles.data(), tileBytes);

        // The compute pass compacts visible source instances into three LOD
        // segments. Each segment has one indirect command whose instanceCount
        // is reset here and incremented atomically by the shader.
        const VkDeviceSize commandStride = sizeof(VkDrawIndexedIndirectCommand);
        const VkDeviceSize commandOffset =
            static_cast<VkDeviceSize>(viewSlot) * 3u * commandStride;
        std::array<VkDrawIndexedIndirectCommand, 3> lodCommands{};
        for (uint32_t lod = 0; lod < 3u; ++lod) {
            lodCommands[lod].indexCount = resource->terrainMdiPatches[lod].indexCount;
            lodCommands[lod].instanceCount = 0;
            lodCommands[lod].firstIndex = 0;
            lodCommands[lod].vertexOffset = 0;
            lodCommands[lod].firstInstance = 0;
        }
        updateBufferInChunks(resource->terrainMdiIndirectBuffers[frame].GetBuffer(),
                             commandOffset, lodCommands.data(),
                             static_cast<VkDeviceSize>(lodCommands.size()) * commandStride);

        VkMemoryBarrier uploadBarrier{};
        uploadBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        uploadBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
                                      VK_ACCESS_TRANSFER_WRITE_BIT;
        uploadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                      VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &uploadBarrier, 0, nullptr, 0, nullptr);

        TerrainMdiCullPush push{};
        for (int plane = 0; plane < 6; ++plane) {
            push.planes[plane] = glm::vec4(cullPlanes[plane].normal,
                                           cullPlanes[plane].distance);
        }
        push.cameraPosDist = glm::vec4(cullCameraPosition, resource->settings.viewDistance);
        push.params = glm::uvec4(
            tileCount,
            static_cast<uint32_t>(viewSlot),
            static_cast<uint32_t>(resource->terrainMdiTileCapacity),
            static_cast<uint32_t>(viewSlot) * static_cast<uint32_t>(resource->terrainMdiTileCapacity));

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_TerrainMdiCullPipeline);
        VkDescriptorSet cullSet = resource->terrainMdiCullDescriptorSets[frame];
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_TerrainMdiCullPipelineLayout, 0, 1, &cullSet,
                                0, nullptr);
        vkCmdPushConstants(commandBuffer, m_TerrainMdiCullPipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(TerrainMdiCullPush), &push);
        vkCmdDispatch(commandBuffer, (tileCount + 63u) / 64u, 1, 1);

        VkMemoryBarrier cullBarrier{};
        cullBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        cullBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        cullBarrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                    VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0, 1, &cullBarrier, 0, nullptr, 0, nullptr);

        cullView.dispatched = true;
        cullView.usesCompactInstances = true;
        cullView.usesCachedCull = useCachedCull;
        cullView.viewProj = viewProj;
    }

}

void TerrainRenderer::CleanupTerrainMdiCullResources() {
    if (g_Device == VK_NULL_HANDLE) {
        m_TerrainMdiCullPipeline = VK_NULL_HANDLE;
        m_TerrainMdiCullPipelineLayout = VK_NULL_HANDLE;
        m_TerrainMdiCullDescriptorPool = VK_NULL_HANDLE;
        m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
        m_TerrainMdiSupported = false;
        m_TerrainMdiMaxDrawCount = 0;
        return;
    }
    if (m_TerrainMdiCullPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_Device, m_TerrainMdiCullPipeline, g_Allocator);
        m_TerrainMdiCullPipeline = VK_NULL_HANDLE;
    }
    if (m_TerrainMdiCullPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_Device, m_TerrainMdiCullPipelineLayout, g_Allocator);
        m_TerrainMdiCullPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_TerrainMdiCullDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_TerrainMdiCullDescriptorPool, g_Allocator);
        m_TerrainMdiCullDescriptorPool = VK_NULL_HANDLE;
    }
    if (m_TerrainMdiCullDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_TerrainMdiCullDescriptorLayout, g_Allocator);
        m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
    }
    m_TerrainMdiSupported = false;
    m_TerrainMdiMaxDrawCount = 0;
}

void TerrainRenderer::RenderInternal(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& projView,
                                     const glm::mat4& prevProjView,
                                     const glm::vec3& cameraPosition,
                                     bool depthOnly,
                                     int viewSlot,
                                     int probeFace) {
    if (commandBuffer == VK_NULL_HANDLE || width <= 0 || height <= 0 || m_PreparedResources.empty()) {
        return;
    }

    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    // viewport/scissor 是 dynamic state，所有管线一致，循环外设置一次。
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    for (Resource* resource : m_PreparedResources) {
        if (!resource) {
            continue;
        }
        // 主渲染按 resource 的 wireframe 标志选择线框/实体管线；
        // 深度 pass 固定使用深度管线（线框只影响颜色显示，不影响深度/CSM）。
        VkPipeline pipeline = m_Pipeline.GetPipeline();
        VkPipelineLayout pipelineLayout = m_Pipeline.GetLayout();
        if (depthOnly) {
            pipeline = m_DepthPipeline.GetPipeline();
            pipelineLayout = m_DepthPipeline.GetLayout();
        } else if (resource->settings.wireframe &&
                   m_WireframePipeline.GetPipeline() != VK_NULL_HANDLE) {
            pipeline = m_WireframePipeline.GetPipeline();
            pipelineLayout = m_WireframePipeline.GetLayout();
        }
        if (pipeline == VK_NULL_HANDLE || pipelineLayout == VK_NULL_HANDLE) {
            continue;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        const int terrainMdiSlot = FindTerrainMdiView(*resource, frame, projView);
        const uint32_t terrainMdiTileCount = terrainMdiSlot >= 0
            ? resource->terrainMdiTileCounts[static_cast<size_t>(terrainMdiSlot)]
            : 0u;
        const bool terrainMdiUsesCompactInstances =
            terrainMdiSlot >= 0 &&
            resource->terrainMdiCullViews[frame][static_cast<size_t>(terrainMdiSlot)]
                .usesCompactInstances;
        const bool useTerrainMdi =
            m_TerrainMdiCullPipeline != VK_NULL_HANDLE &&
            terrainMdiSlot >= 0 &&
            (terrainMdiTileCount == 0 ||
             (resource->terrainMdiTileCapacity >= terrainMdiTileCount &&
              (terrainMdiUsesCompactInstances
                   ? resource->terrainMdiCompactInstanceBuffers[frame].GetBuffer()
                   : resource->terrainMdiInstanceBuffers[frame].GetBuffer()) != VK_NULL_HANDLE &&
              resource->terrainMdiIndirectBuffers[frame].GetBuffer() != VK_NULL_HANDLE));
        if (TerrainDiagEnabled()) {
            LOGI("[TerrainDiag] DrawTerrain frame=%u slot=%d tiles=%u useMdi=%d cap=%zu "
                 "cachedCull=%d",
                 frame, terrainMdiSlot, terrainMdiTileCount, useTerrainMdi ? 1 : 0,
                 resource->terrainMdiTileCapacity,
                 resource->terrainMdiUseFrustumCulling ? 1 : 0);
        }
        if (useTerrainMdi) {
            UpdateUniform(*resource, projView, prevProjView, cameraPosition,
                          true, viewSlot, probeFace);

            const VkDescriptorSet viewSet =
                ViewDescriptorSet(*resource, frame, viewSlot, probeFace);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 0, 1, &viewSet,
                                    0, nullptr);

            const VkDeviceSize instanceSegmentBytes =
                static_cast<VkDeviceSize>(resource->terrainMdiTileCapacity) *
                sizeof(TerrainChunkInstance);
            const VkDeviceSize commandStride = sizeof(VkDrawIndexedIndirectCommand);

            if (terrainMdiTileCount > 0) {
                for (uint32_t lod = 0; lod < 3u; ++lod) {
                    const uint32_t lodTileCount =
                        resource->terrainMdiLodTileCounts[
                            static_cast<size_t>(terrainMdiSlot)][lod];
                    if (lodTileCount == 0) {
                        continue;
                    }
                    VkBuffer instanceBuffer = terrainMdiUsesCompactInstances
                        ? resource->terrainMdiCompactInstanceBuffers[frame].GetBuffer()
                        : resource->terrainMdiInstanceBuffers[frame].GetBuffer();
                    VkBuffer vertexBuffers[2] = {
                        resource->terrainMdiPatches[lod].vertexBuffer.GetBuffer(),
                        instanceBuffer
                    };
                    const VkDeviceSize instanceOffset = terrainMdiUsesCompactInstances
                        ? (static_cast<VkDeviceSize>(terrainMdiSlot) * 3u + lod) *
                              instanceSegmentBytes
                        : (static_cast<VkDeviceSize>(terrainMdiSlot) * instanceSegmentBytes +
                           static_cast<VkDeviceSize>(resource->terrainMdiLodInstanceOffsets[
                               static_cast<size_t>(terrainMdiSlot)][lod]) *
                               sizeof(TerrainChunkInstance));
                    VkDeviceSize offsets[2] = {0, instanceOffset};
                    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
                    vkCmdBindIndexBuffer(commandBuffer,
                                         resource->terrainMdiPatches[lod].indexBuffer.GetBuffer(),
                                         0, VK_INDEX_TYPE_UINT32);
                    const VkDeviceSize commandOffset =
                        (static_cast<VkDeviceSize>(terrainMdiSlot) * 3u + lod) *
                        commandStride;
                    vkCmdDrawIndexedIndirect(commandBuffer,
                                             resource->terrainMdiIndirectBuffers[frame].GetBuffer(),
                                             commandOffset, 1u,
                                             static_cast<uint32_t>(commandStride));
                }
            }

            if (!depthOnly) {
                RenderGrass(commandBuffer, *resource, frame, projView,
                            viewSlot, probeFace);
                resource->previousModel = resource->model;
                resource->hasPreviousModel = true;
            }
            continue;
        }

        const bool probeView = IsProbeViewSlot(viewSlot);
        const size_t visibleCount = resource->chunks.GetVisibleCount();
        if (visibleCount == 0 ||
            !EnsureInstanceCapacity(*resource, visibleCount, probeView)) {
            continue;
        }

        std::vector<TerrainChunkInstance> instances;
        instances.reserve(visibleCount);
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            const float edgeIntervals = static_cast<float>(
                std::max(resource->patches[static_cast<size_t>(lod)].resolution, 2u) - 1u);
            for (TerrainChunkInstance instance : visible) {
                instance.params.z = edgeIntervals;
                instances.push_back(instance);
            }
        }
        // 实例流按视图分流：Write 是 host memcpy（无命令流排序），探针在帧尾的
        // 写入若落进主视图那份，会盖掉主视图已录制命令要读的实例数据。
        VulkanBuffer& viewInstanceBuffer = probeView
            ? resource->probeInstanceBuffers[frame][static_cast<size_t>(
                  std::clamp(probeFace, 0, kProbeFaceCount - 1))]
            : resource->instanceBuffers[frame];
        viewInstanceBuffer.Write(
            instances.data(), static_cast<VkDeviceSize>(instances.size() * sizeof(TerrainChunkInstance)));
        UpdateUniform(*resource, projView, prevProjView, cameraPosition,
                      true, viewSlot, probeFace);

        const VkDescriptorSet viewSet =
            ViewDescriptorSet(*resource, frame, viewSlot, probeFace);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &viewSet, 0, nullptr);

        uint32_t firstInstance = 0;
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            if (visible.empty()) {
                continue;
            }

            VkBuffer vertexBuffers[2] = {
                resource->patches[static_cast<size_t>(lod)].vertexBuffer.GetBuffer(),
                viewInstanceBuffer.GetBuffer()
            };
            VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer,
                                 resource->patches[static_cast<size_t>(lod)].indexBuffer.GetBuffer(),
                                 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(commandBuffer,
                             resource->patches[static_cast<size_t>(lod)].indexCount,
                             static_cast<uint32_t>(visible.size()),
                             0, 0, firstInstance);
            firstInstance += static_cast<uint32_t>(visible.size());
        }

        if (!depthOnly) {
            // 草画在地形之后、同一 G-buffer subpass：地形已写深度，
            // 叶片是不透明细三角形（无混合），深度测试自然裁掉遮挡。
            RenderGrass(commandBuffer, *resource, frame, projView,
                        viewSlot, probeFace);

            // 水面已移出 G-buffer（deferred water compositing）：由
            // SceneRenderer::RenderWaterTargets 在几何 pass 后写独立目标 RT。

            resource->previousModel = resource->model;
            resource->hasPreviousModel = true;
        }
    }
}

std::string TerrainRenderer::MakeTextureKey(ECS::Entity entity, const char* slot) {
    return "__terrain_" + std::to_string(static_cast<uint32_t>(entity)) + "_" + slot;
}

const std::string& TerrainRenderer::GetLayerPath(const ECS::TerrainComponent& settings, int layer) {
    switch (layer) {
    case 0: return settings.layer0Path;
    case 1: return settings.layer1Path;
    case 2: return settings.layer2Path;
    case 3: return settings.layer3Path;
    default: {
        static const std::string empty;
        return empty;
    }
    }
}
