#include "TerrainRenderer.h"

#include "Core/EngineConfig.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "ECS/SceneECS.h"
#include "Rendering/RenderWorld.h"
#include "Rendering/HeightmapLoader.h"
#include "TexturePool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <SDL3/SDL_timer.h>
#include "Rendering/RenderStats.h"
#include "Core/Log.h"

namespace {

constexpr size_t kInitialDescriptorSets = 512;

// 程序化平坦高度图：heightmapPath 为空时地形默认是一张平面。
// 512 在 256 世界单位下约 0.5 单位/texel，对编辑器笔刷粒度足够。
constexpr uint32_t kProceduralHeightmapResolution = 512;
// 平坦基准取归一化中值，升高/降低两个方向都留有余量；
// 配合预设里的 heightOffset = -heightScale/2，平坦面正好落在局部 y = 0。
constexpr uint16_t kProceduralFlatSample = 32768;

// 草可见距离（米）：超出后顶点着色器把叶片收缩到相机外。草叶高约 0.85m，
// 100m 处在 1080p 下不足 2 像素——更远的草只有像素级 overdraw 没有信息量，
// 直接不画。密度衰减（45% 视距起 hash 逐株抽稀）+ 视距内整体溶解都在
// grass.vert 里做，主 pass 与阴影 pass 严格一致。
constexpr float kGrassViewDistance = 100.0f;
// 目标草密度（株/平方米，密度 255 时）。散布按 texel 的世界面积换算，
// 与密度图分辨率无关：256² 与 1024² 的密度图在同一个世界里长出同样密的草。
constexpr float kGrassBladesPerM2 = 36.0f;
// 实例流硬上限（32B/株 → 1M ≈ 32MB/帧槽位），超预算按全局稀释兜底。
constexpr uint32_t kGrassMaxInstances = 1000000;

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
    for (auto& visible : m_Visible) {
        visible.clear();
    }

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
                                           bool useFrustumCulling) {
    for (auto& visible : m_Visible) {
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
        m_Visible[static_cast<size_t>(lod)].push_back(instance);
    }
}

const std::vector<TerrainChunkInstance>& TerrainChunkManager::GetVisible(int lod) const {
    static const std::vector<TerrainChunkInstance> empty;
    if (lod < 0 || lod >= static_cast<int>(m_Visible.size())) {
        return empty;
    }
    return m_Visible[static_cast<size_t>(lod)];
}

size_t TerrainChunkManager::GetVisibleCount() const {
    size_t count = 0;
    for (const auto& visible : m_Visible) {
        count += visible.size();
    }
    return count;
}

TerrainRenderer::~TerrainRenderer() {
    Cleanup();
}

void TerrainRenderer::Init(VkRenderPass renderPass) {
    m_RenderPass = renderPass;

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
    m_CsmRenderPass = VK_NULL_HANDLE;
    DestroyDescriptorResources();

    if (m_OwnedWhiteFallback && g_TexturePool) {
        g_TexturePool->Release("white");
    }
    m_OwnedWhiteFallback = false;
    m_RenderPass = VK_NULL_HANDLE;
}

void TerrainRenderer::Prepare(const std::vector<ECS::Entity>& rootEntities,
                              const glm::vec3& cameraPosition,
                              const std::array<Plane, 6>& frustumPlanes,
                              bool useFrustumCulling) {
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
        resource->model = sceneECS.GetWorldMatrix(entity);
        resource->chunks.UpdateVisibility(cameraPosition, resource->model,
                                          frustumPlanes, useFrustumCulling);
        resource->grassFrustumPlanes = frustumPlanes;
        resource->grassUseFrustumCulling = useFrustumCulling;
        resource->grassCameraPosition = cameraPosition;
        m_PreparedResources.push_back(resource);
        m_VisibleChunkCount += resource->chunks.GetVisibleCount();
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
                              bool useFrustumCulling) {
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
        resource->model = entityData->transform.worldMatrix;
        resource->chunks.UpdateVisibility(cameraPosition, resource->model,
                                          frustumPlanes, useFrustumCulling);
        resource->grassFrustumPlanes = frustumPlanes;
        resource->grassUseFrustumCulling = useFrustumCulling;
        resource->grassCameraPosition = cameraPosition;
        m_PreparedResources.push_back(resource);
        m_VisibleChunkCount += resource->chunks.GetVisibleCount();
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
                                       bool useFrustumCulling) {
    Prepare(ECS::SceneECS::GetInstance().GetRootEntities(), cameraPosition,
            frustumPlanes, useFrustumCulling);
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

    // 高度图来源两条路径，共同点是都保留一份 CPU 镜像：地形笔刷只改镜像再局部上传，
    // 因此不需要 PNG 编码器，也不会因为改像素而触发资源重建（SettingsEqual 只看路径）。
    if (settings.heightmapPath.empty()) {
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
        const std::string heightmapPath = EngineConfig::GetFullPath(settings.heightmapPath.c_str());
        HeightmapPixels16 pixels;
        std::string errorMessage;
        if (!HeightmapLoader::LoadPng16(heightmapPath, pixels, &errorMessage)) {
            LOGE("[TerrainRenderer] failed to load heightmap for entity %u: %s (%s)",
                        static_cast<unsigned>(entity), settings.heightmapPath.c_str(),
                        errorMessage.c_str());
            return nullptr;
        }
        resource->heightmapWidth = pixels.width;
        resource->heightmapHeight = pixels.height;
        resource->heightmapCpu = std::move(pixels.samples);
        if (!g_TexturePool->CreateHeightmap16FromMemory(
                resource->heightmapKey, resource->heightmapWidth, resource->heightmapHeight,
                resource->heightmapCpu.data(), SamplerType::LinearClamp)) {
            LOGE("[TerrainRenderer] failed to upload heightmap for entity %u: %s",
                        static_cast<unsigned>(entity), settings.heightmapPath.c_str());
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

        if (settings.controlMapPath.empty()) {
            resource->controlProcedural = true;
            buildFromMemory = true;
        } else {
            const std::string controlPath = EngineConfig::GetFullPath(settings.controlMapPath.c_str());
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

    // 草密度图（R8，与高度图同分辨率）：一律从内存建、零初始化（默认无草），
    // 草地笔刷只改 CPU 镜像再局部回写，散布实例按镜像重建。
    resource->grassKey = MakeTextureKey(entity, "grass");
    resource->grassWidth = resource->heightmapWidth;
    resource->grassHeight = resource->heightmapHeight;
    resource->grassCpu.assign(static_cast<size_t>(resource->grassWidth) *
                                  resource->grassHeight, 0);
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

    return resource;
}

void TerrainRenderer::DestroyResource(Resource& resource) {
    if (m_DescriptorPool != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        std::vector<VkDescriptorSet> sets;
        sets.reserve(resource.descriptorSets.size());
        for (VkDescriptorSet& set : resource.descriptorSets) {
            if (set != VK_NULL_HANDLE) {
                sets.push_back(set);
                set = VK_NULL_HANDLE;
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
    for (auto& instanceBuffer : resource.instanceBuffers) {
        instanceBuffer.Cleanup();
    }
    for (auto& instanceBuffer : resource.csmInstanceBuffers) {
        instanceBuffer.Cleanup();
    }
    resource.csmInstanceCapacity = 0;
    for (auto& grassBuffer : resource.grassInstanceBuffers) {
        grassBuffer.Cleanup();
    }
    resource.grassInstanceCapacity = 0;
    resource.grassInstanceCount = 0;
    for (auto& patch : resource.patches) {
        patch.Cleanup();
    }

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

    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        if (i == 1) {
            // 高度图同时供顶点位移和片元级连续法线/坡度计算使用。
            bindings[i].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
        }
    }

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
    poolSizes[0].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets);
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets * 6);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = static_cast<uint32_t>(kInitialDescriptorSets);
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
        grassConfig.subpass = 1;
        grassConfig.vertexBindings.assign(grassBindings.begin(), grassBindings.end());
        grassConfig.vertexAttributes.assign(grassAttributes.begin(), grassAttributes.end());
        if (!m_GrassPipeline.Create(m_RenderPass, m_DescriptorLayout, grassConfig)) {
            // 草地管线失败只降级草地渲染，不影响地形本体。
            LOGE("[TerrainRenderer] grass pipeline creation failed - grass rendering disabled");
        }
    }
    return true;
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

bool TerrainRenderer::CreateInstanceBuffers(Resource& resource, size_t capacity) {
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    resource.instanceCapacity = std::max<size_t>(1, capacity);
    for (auto& buffer : resource.instanceBuffers) {
        buffer.Cleanup();
        if (!buffer.Create(static_cast<VkDeviceSize>(resource.instanceCapacity * sizeof(TerrainChunkInstance)),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
            for (auto& cleanup : resource.instanceBuffers) {
                cleanup.Cleanup();
            }
            resource.instanceCapacity = 0;
            return false;
        }
    }
    return true;
}

bool TerrainRenderer::CreateUniformBuffers(Resource& resource) {
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (auto& uniform : resource.uniformBuffers) {
        uniform = std::make_unique<VulkanBuffer>();
        if (!uniform->Create(sizeof(TerrainUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory)) {
            for (auto& cleanup : resource.uniformBuffers) {
                cleanup.reset();
            }
            return false;
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
    allocInfo.descriptorSetCount = kFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(g_Device, &allocInfo, resource.descriptorSets.data()) != VK_SUCCESS) {
        resource.descriptorSets.fill(VK_NULL_HANDLE);
        return false;
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

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = resource.uniformBuffers[frame]->GetBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(TerrainUniformData);

        std::array<VkDescriptorImageInfo, 6> images{};
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

        std::array<VkWriteDescriptorSet, 7> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = resource.descriptorSets[frame];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        for (uint32_t binding = 1; binding < writes.size(); ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = resource.descriptorSets[frame];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &images[binding - 1];
        }
        vkUpdateDescriptorSets(g_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return true;
}

bool TerrainRenderer::EnsureInstanceCapacity(Resource& resource, size_t visibleCount) {
    if (visibleCount <= resource.instanceCapacity) {
        return true;
    }
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    const size_t newCapacity = std::max(visibleCount, std::max<size_t>(1, resource.instanceCapacity * 2));
    return CreateInstanceBuffers(resource, newCapacity);
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
                                    bool applyTAAJitter) {
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
                                       0.0f);
    uniform.cameraPosition = glm::vec4(cameraPosition, 1.0f);
    uniform.taaJitter = applyTAAJitter
        ? glm::vec4(g_CurrentTAAJitter, 0.0f, 0.0f)
        : glm::vec4(0.0f);
    uniform.timeWind = glm::vec4(static_cast<float>(SDL_GetTicks()) * 0.001f,
                                 1.0f, kGrassViewDistance, 1.0f);
    resource.uniformBuffers[frame]->Write(&uniform, sizeof(uniform));
}

void TerrainRenderer::Render(VkCommandBuffer commandBuffer, int width, int height,
                             const glm::mat4& projView,
                             const glm::mat4& prevProjView,
                             const glm::vec3& cameraPosition) {
    RenderInternal(commandBuffer, width, height, projView, prevProjView, cameraPosition, false);
}

void TerrainRenderer::RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                                         const glm::mat4& projView,
                                         const glm::vec3& cameraPosition) {
    RenderInternal(commandBuffer, width, height, projView, projView, cameraPosition, true);
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
        UpdateUniform(*resource, shadowProjView, shadowProjView, cameraPosition);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &resource->descriptorSets[frame], 0, nullptr);

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

// 整数散列（pcg 风格 finalizer）：散布草叶用的确定性伪随机。
// 同一密度图 + 同一 texel 永远产出同一批叶片位置，重建成幂等。
inline uint32_t GrassHash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline float GrassHashFloat(uint32_t h) {
    // [0, 1)
    return static_cast<float>(h & 0x00ffffffU) / 16777216.0f;
}

void TerrainRenderer::RebuildGrassInstances(Resource& resource) {
    resource.grassDirty = false;
    resource.grassStaging.clear();
    if (resource.grassCpu.empty() || resource.grassWidth < 2 || resource.grassHeight < 2) {
        resource.grassInstanceCount = 0;
        return;
    }

    const glm::vec2 worldSize = resource.settings.worldSize;
    const uint32_t grassWidth = resource.grassWidth;
    const uint32_t grassHeight = resource.grassHeight;

    // 第一遍：统计满密度需求，算全局稀释比例。上限是全体预算——超预算时
    // 按比例稀释而不是按行截断，否则整图涂草会变成"远侧有草近侧秃"。
    // 每株数按 texel 世界面积换算：密度语义 = 株/平方米，与密度图分辨率无关。
    const float texelWorldX = worldSize.x / std::max(static_cast<float>(grassWidth - 1), 1.0f);
    const float texelWorldZ = worldSize.y / std::max(static_cast<float>(grassHeight - 1), 1.0f);
    const float texelArea = std::max(texelWorldX * texelWorldZ, 1e-6f);
    const float fullTexelBlades = kGrassBladesPerM2 * texelArea;
    uint64_t grassDemand = 0;
    for (uint32_t y = 0; y < grassHeight; ++y) {
        for (uint32_t x = 0; x < grassWidth; ++x) {
            const uint8_t density = resource.grassCpu[static_cast<size_t>(y) * grassWidth + x];
            if (density == 0) {
                continue;
            }
            grassDemand += static_cast<uint64_t>(
                static_cast<float>(density) * fullTexelBlades / 255.0f + 0.5f);
        }
    }
    const float grassThinScale =
        grassDemand > kGrassMaxInstances
            ? static_cast<float>(kGrassMaxInstances) / static_cast<float>(grassDemand)
            : 1.0f;
    if (grassThinScale < 1.0f) {
        LOGW("[TerrainRenderer] grass demand %llu blades exceeds cap %u, "
                    "uniformly thinned to %.1f%%",
                    static_cast<unsigned long long>(grassDemand), kGrassMaxInstances,
                    grassThinScale * 100.0f);
    }

    for (uint32_t y = 0; y < grassHeight; ++y) {
        for (uint32_t x = 0; x < grassWidth; ++x) {
            const uint8_t density = resource.grassCpu[static_cast<size_t>(y) * grassWidth + x];
            if (density == 0) {
                continue;
            }
            // 密度 255 → kGrassBladesPerM2 * texelArea 株；低密度按比例舍入。
            const uint32_t bladeCount = std::min<uint32_t>(
                4096u, static_cast<uint32_t>(
                    static_cast<float>(density) * fullTexelBlades / 255.0f + 0.5f));
            uint32_t placed = bladeCount;
            if (grassThinScale < 1.0f) {
                // 稀释在 texel 粒度做 hash 抖动取整：小数部分按概率归入，
                // 保证整图密度均匀下降，而不是每 texel 都砍尾。
                const float scaled = static_cast<float>(bladeCount) * grassThinScale;
                placed = static_cast<uint32_t>(scaled);
                if (GrassHashFloat(GrassHash((y * 92837111u) ^ (x * 689287499u))) <
                    scaled - static_cast<float>(placed)) {
                    ++placed;
                }
                placed = std::min(placed, bladeCount);
            }

            for (uint32_t slot = 0; slot < placed; ++slot) {
                if (resource.grassStaging.size() >= kGrassMaxInstances) {
                    LOGW("[TerrainRenderer] grass instance cap reached (%u), "
                                "rest of the map is not scattered", kGrassMaxInstances);
                    resource.grassStaging.shrink_to_fit();
                    resource.grassInstanceCount =
                        static_cast<uint32_t>(resource.grassStaging.size());
                    FinalizeGrassBuckets(resource);
                    return;
                }

                const uint32_t seed = GrassHash(
                    (y * 73856093u) ^ (x * 19349663u) ^ (slot * 83492791u));
                const float offsetX = GrassHashFloat(seed);
                const float offsetZ = GrassHashFloat(GrassHash(seed + 0x68bc21ebu));
                const float randA = GrassHashFloat(GrassHash(seed + 2u));
                const float randB = GrassHashFloat(GrassHash(seed + 3u));
                const float randC = GrassHashFloat(GrassHash(seed + 4u));

                // texel 内偏移采样点：镜像行序（顶左原点）→ 地形局部 XZ。
                const float u = (static_cast<float>(x) + offsetX) / static_cast<float>(grassWidth);
                const float v = (static_cast<float>(y) + offsetZ) / static_cast<float>(grassHeight);
                const float localX = (u - 0.5f) * worldSize.x;
                const float localZ = (0.5f - v) * worldSize.y;

                GrassBladeInstance blade;
                blade.posParams = glm::vec4(localX, localZ,
                                            randA * 6.2831853f,              // yaw
                                            0.35f + randB * 0.5f);           // height (m)
                blade.shapeParams = glm::vec4(0.028f + randC * 0.045f,       // width (m)
                                              (randB - 0.5f) * 0.8f,         // bend
                                              randA * 6.2831853f,            // phase
                                              randC);                        // tint
                resource.grassStaging.push_back(blade);
            }
        }
    }
    resource.grassInstanceCount = static_cast<uint32_t>(resource.grassStaging.size());
    FinalizeGrassBuckets(resource);
}

// 把散布好的草实例流按"区块细分格"稳定分桶（计数排序，桶内保持原顺序）。
// 每个地形 chunk 再切 kGrassBucketSubdiv×kGrassBucketSubdiv 个子格
// （=2 时每桶是区块面积的 1/4），比地形剔除粒度更细：部分进视锥的
// chunk 只有真正可见的子格才发 vkCmdDraw。归一化边界公式与
// TerrainChunkManager 同源，X/Z 除以总格数即可对齐世界空间。
// 每桶 Y 范围直接扫高度图 CPU 镜像的对应矩形（外扩 1 texel 覆盖
// 双线性采样的邻域），比整块地形共用一套全局高度范围能得到紧凑得
// 多的包围盒，斜坡背面的桶更容易被剔掉。
void TerrainRenderer::FinalizeGrassBuckets(Resource& resource) {
    resource.grassBuckets.clear();
    if (resource.grassStaging.empty()) {
        return;
    }

    // 区块内细分：1 = 与地形 chunk 同粒度；2 = 每区块 2×2 子格（1/4 大小）。
    constexpr int kGrassBucketSubdiv = 2;
    const glm::vec2 worldSize = glm::max(resource.settings.worldSize, glm::vec2(1e-3f));
    const int chunkCount = std::clamp(resource.settings.chunkCount, 1, 256);
    const int bucketCount = std::min(chunkCount * kGrassBucketSubdiv, 512);
    const size_t bucketTotal = static_cast<size_t>(bucketCount) * static_cast<size_t>(bucketCount);

    std::vector<uint32_t> bucketOf(resource.grassStaging.size());
    std::vector<uint32_t> bucketSizes(bucketTotal, 0);
    for (size_t i = 0; i < resource.grassStaging.size(); ++i) {
        const GrassBladeInstance& blade = resource.grassStaging[i];
        const float nx = (blade.posParams.x + worldSize.x * 0.5f) / worldSize.x;
        const float nz = (blade.posParams.y + worldSize.y * 0.5f) / worldSize.y;
        const int cx = std::clamp(static_cast<int>(nx * static_cast<float>(bucketCount)),
                                  0, bucketCount - 1);
        const int cz = std::clamp(static_cast<int>(nz * static_cast<float>(bucketCount)),
                                  0, bucketCount - 1);
        const uint32_t index = static_cast<uint32_t>(cz) * static_cast<uint32_t>(bucketCount) +
                               static_cast<uint32_t>(cx);
        bucketOf[i] = index;
        ++bucketSizes[index];
    }

    const float heightScale = resource.settings.heightScale;
    const float heightOffset = resource.settings.heightOffset;
    const bool hasHeightMirror = !resource.heightmapCpu.empty() &&
                                 resource.heightmapWidth >= 2 && resource.heightmapHeight >= 2;
    constexpr float kLeafHeightMargin = 2.0f;   // 叶高 0.85m + 风摆/增益余量
    constexpr float kGroundMargin = 0.5f;       // 根部贴地，向下只留采样余量

    resource.grassBuckets.resize(bucketTotal);
    std::vector<uint32_t> writeCursor(bucketTotal, 0);
    uint32_t running = 0;
    for (size_t b = 0; b < bucketTotal; ++b) {
        GrassChunkBucket& bucket = resource.grassBuckets[b];
        bucket.firstInstance = running;
        bucket.count = bucketSizes[b];
        writeCursor[b] = running;
        running += bucketSizes[b];

        const int cx = static_cast<int>(b % static_cast<size_t>(bucketCount));
        const int cz = static_cast<int>(b / static_cast<size_t>(bucketCount));
        const float invCount = 1.0f / static_cast<float>(bucketCount);
        const glm::vec2 normMin(static_cast<float>(cx) * invCount,
                                static_cast<float>(cz) * invCount);
        const glm::vec2 normMax(static_cast<float>(cx + 1) * invCount,
                                static_cast<float>(cz + 1) * invCount);
        const glm::vec2 origin = -worldSize * 0.5f + normMin * worldSize;
        const glm::vec2 size = (normMax - normMin) * worldSize;

        float yLow = std::min(heightOffset, heightOffset + heightScale) - kGroundMargin;
        float yHigh = std::max(heightOffset, heightOffset + heightScale) + kLeafHeightMargin;
        if (hasHeightMirror) {
            // 扫桶对应的高度图矩形（外扩 1 texel：桶内任意点的双线性采样
            // 最远只读到相邻 texel），取 min/max 换算世界高度。
            const uint32_t hw = resource.heightmapWidth;
            const uint32_t hh = resource.heightmapHeight;
            const uint32_t x0 = std::max<int>(0, (static_cast<int>(cx) * static_cast<int>(hw)) / bucketCount - 1);
            const uint32_t x1 = std::min<uint32_t>(hw, ((static_cast<int>(cx) + 1) * static_cast<int>(hw)) / bucketCount + 1);
            const uint32_t z0 = std::max<int>(0, (static_cast<int>(cz) * static_cast<int>(hh)) / bucketCount - 1);
            const uint32_t z1 = std::min<uint32_t>(hh, ((static_cast<int>(cz) + 1) * static_cast<int>(hh)) / bucketCount + 1);
            uint16_t minV = 0xffffu;
            uint16_t maxV = 0;
            for (uint32_t z = z0; z < z1; ++z) {
                const uint16_t* row = &resource.heightmapCpu[static_cast<size_t>(z) * hw];
                for (uint32_t x = x0; x < x1; ++x) {
                    minV = std::min(minV, row[x]);
                    maxV = std::max(maxV, row[x]);
                }
            }
            if (minV <= maxV) {
                yLow = heightOffset + (static_cast<float>(minV) / 65535.0f) * heightScale - kGroundMargin;
                yHigh = heightOffset + (static_cast<float>(maxV) / 65535.0f) * heightScale + kLeafHeightMargin;
            }
        }
        bucket.localBounds = AABB(glm::vec3(origin.x, yLow, origin.y),
                                  glm::vec3(origin.x + size.x, yHigh, origin.y + size.y));
    }

    // 稳定重排：实例流按桶连续存放，桶内保持散布顺序（确定性不变）。
    std::vector<GrassBladeInstance> sorted(resource.grassStaging.size());
    for (size_t i = 0; i < resource.grassStaging.size(); ++i) {
        sorted[writeCursor[bucketOf[i]]++] = resource.grassStaging[i];
    }
    resource.grassStaging.swap(sorted);
}

uint32_t TerrainRenderer::EnsureGrassInstancesUploaded(Resource& resource, uint32_t frame) {
    if (resource.grassDirty) {
        RebuildGrassInstances(resource);
        if (resource.grassInstanceCount > 0) {
            if (resource.grassInstanceCount > resource.grassInstanceCapacity) {
                if (g_Device != VK_NULL_HANDLE) {
                    vkDeviceWaitIdle(g_Device);
                }
                const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                uint32_t newCapacity = resource.grassInstanceCapacity;
                while (newCapacity < resource.grassInstanceCount) {
                    newCapacity = std::max(1024u, newCapacity * 2u);
                }
                bool created = true;
                for (auto& buffer : resource.grassInstanceBuffers) {
                    buffer.Cleanup();
                    created = created && buffer.Create(
                        static_cast<VkDeviceSize>(newCapacity * sizeof(GrassBladeInstance)),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory);
                }
                resource.grassInstanceCapacity = created ? newCapacity : 0;
                if (!created) {
                    LOGE("[TerrainRenderer] grass instance buffer creation failed");
                    resource.grassInstanceCount = 0;
                    return 0;
                }
            }
            const VkDeviceSize bytes =
                static_cast<VkDeviceSize>(resource.grassInstanceCount) * sizeof(GrassBladeInstance);
            // 写满全部帧槽位：重建不与具体帧绑定，避免槽位间数据陈旧不一致。
            for (auto& buffer : resource.grassInstanceBuffers) {
                buffer.Write(resource.grassStaging.data(), bytes);
            }
        }
    }
    if (resource.grassInstanceCount == 0 ||
        resource.grassInstanceBuffers[frame].GetBuffer() == VK_NULL_HANDLE) {
        return 0;
    }
    return resource.grassInstanceCount;
}

void TerrainRenderer::RenderGrass(VkCommandBuffer commandBuffer, Resource& resource,
                                  uint32_t frame) {
    if (m_GrassPipeline.GetPipeline() == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t instanceCount = EnsureGrassInstancesUploaded(resource, frame);
    if (instanceCount == 0) {
        return;
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_GrassPipeline.GetPipeline());
    // 主 pass 模式：useCsm=0，grass.vert 用 UBO 的 projView（含 TAA 抖动）。
    {
        struct GrassPushData {
            glm::mat4 csmProjView;
            glm::vec4 csmParams;
        } pushData{glm::mat4(1.0f), glm::vec4(0.0f)};
        vkCmdPushConstants(commandBuffer, m_GrassPipeline.GetLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GrassPushData), &pushData);
    }
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_GrassPipeline.GetLayout(), 0, 1,
                            &resource.descriptorSets[frame], 0, nullptr);
    VkBuffer instanceBuffer = resource.grassInstanceBuffers[frame].GetBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &instanceBuffer, &offset);
    // 10 顶点 = 4 段条带（2*(段数+1)），零顶点缓冲，几何全在顶点着色器里生成。
    // 逐桶做视锥 + 距离剔除：Prepare 时随 chunk 可见性缓存进 Resource。
    RenderGrassBuckets(commandBuffer, resource,
                       resource.grassFrustumPlanes, resource.grassUseFrustumCulling,
                       resource.grassCameraPosition);
}

void TerrainRenderer::RenderGrassCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                                          const glm::mat4& shadowProjView,
                                          const glm::vec3& cameraPosition) {
    if (commandBuffer == VK_NULL_HANDLE || width <= 0 || height <= 0 ||
        m_PreparedResources.empty() || m_GrassDepthPipeline.GetPipeline() == VK_NULL_HANDLE) {
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

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_GrassDepthPipeline.GetPipeline());
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    // 阴影 pass 模式：useCsm=1，grass.vert 用 push constant 的级联矩阵。
    struct GrassPushData {
        glm::mat4 csmProjView;
        glm::vec4 csmParams;
    } grassPushData{shadowProjView, glm::vec4(1.0f)};

    for (Resource* resource : m_PreparedResources) {
        if (!resource) {
            continue;
        }
        // 阴影 pass 在主 pass 之前，这里负责把散布结果先上传（含首帧）。
        const uint32_t instanceCount = EnsureGrassInstancesUploaded(*resource, frame);
        if (instanceCount == 0) {
            continue;
        }
        // 光矩阵经 push constant 传入（useCsm=1）：一帧内多个级联 + 主 pass
        // 共享同一份 per-frame UBO，录制期的 UBO 写入只有最后一次生效——
        // 级联矩阵写 UBO 会让草深度全部拿到主相机的 projView（草影消失的
        // 根因）。UBO 仍提供 model/高度/相机/风摆（与主 pass 一致）。
        UpdateUniform(*resource, shadowProjView, shadowProjView, cameraPosition, false);
        vkCmdPushConstants(commandBuffer, m_GrassDepthPipeline.GetLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(glm::mat4) + sizeof(glm::vec4),
                           &grassPushData);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_GrassDepthPipeline.GetLayout(), 0, 1,
                                &resource->descriptorSets[frame], 0, nullptr);
        VkBuffer instanceBuffer = resource->grassInstanceBuffers[frame].GetBuffer();
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &instanceBuffer, &offset);
        // 草的阴影投射按当前级联的光视锥逐桶剔除：从光空间 projView 现场提取
        // 6 平面（ortho 与透视矩阵通用），与地形 CSM 的实例级准备相互独立。
        const std::array<Plane, 6> lightPlanes = AABBUtils::ExtractFrustumPlanes(shadowProjView);
        RenderGrassBuckets(commandBuffer, *resource, lightPlanes, true, cameraPosition);
    }
}

void TerrainRenderer::RenderGrassBuckets(VkCommandBuffer commandBuffer, Resource& resource,
                                         const std::array<Plane, 6>& frustumPlanes,
                                         bool useFrustumCulling, const glm::vec3& cameraPosition) {
    for (const GrassChunkBucket& bucket : resource.grassBuckets) {
        if (bucket.count == 0) {
            continue;
        }
        const AABB worldBounds = bucket.localBounds.Transform(resource.model);
        // 距离剔除：桶 AABB 最近点到相机的水平距离超出视距即整桶不发——
        // 桶内叶片在顶点着色器里也会被视距剔掉，这里省掉整桶的 VS 调用。
        if (useFrustumCulling) {
            const glm::vec2 camXZ(cameraPosition.x, cameraPosition.z);
            const glm::vec2 closest = glm::clamp(camXZ,
                                                 glm::vec2(worldBounds.min.x, worldBounds.min.z),
                                                 glm::vec2(worldBounds.max.x, worldBounds.max.z));
            if (glm::distance(camXZ, closest) > kGrassViewDistance) {
                continue;
            }
            if (!worldBounds.IsInsideFrustum(frustumPlanes)) {
                continue;
            }
        }
        vkCmdDraw(commandBuffer, 10, bucket.count, 0, bucket.firstInstance);
    }
}

void TerrainRenderer::RenderInternal(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& projView,
                                     const glm::mat4& prevProjView,
                                     const glm::vec3& cameraPosition,
                                     bool depthOnly) {
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

        const size_t visibleCount = resource->chunks.GetVisibleCount();
        if (visibleCount == 0 || !EnsureInstanceCapacity(*resource, visibleCount)) {
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
        resource->instanceBuffers[frame].Write(
            instances.data(), static_cast<VkDeviceSize>(instances.size() * sizeof(TerrainChunkInstance)));
        UpdateUniform(*resource, projView, prevProjView, cameraPosition);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &resource->descriptorSets[frame], 0, nullptr);

        uint32_t firstInstance = 0;
        for (int lod = 0; lod < 3; ++lod) {
            const auto& visible = resource->chunks.GetVisible(lod);
            if (visible.empty()) {
                continue;
            }

            VkBuffer vertexBuffers[2] = {
                resource->patches[static_cast<size_t>(lod)].vertexBuffer.GetBuffer(),
                resource->instanceBuffers[frame].GetBuffer()
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
            RenderGrass(commandBuffer, *resource, frame);
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

// ===== 编辑器地形笔刷 =====
// 所有编辑几何都在地形局部空间完成：地形实体的 model 矩阵可能带旋转与缩放，
// 在世界空间直接做会引入非均匀缩放误差。局部 XZ 的规则网格以原点为中心，
// 范围 [-worldSize/2, +worldSize/2]，高度为 heightOffset + normalized*heightScale。
namespace {

// 采样高度图所需的最小上下文，避免匿名命名空间依赖 private 的 Resource 类型。
struct HeightmapSampling {
    const uint16_t* samples = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    glm::vec2 worldSize = glm::vec2(1.0f);
    float heightScale = 1.0f;
    float heightOffset = 0.0f;
};

glm::vec3 WorldToTerrainLocal(const glm::mat4& model, const glm::vec3& world) {
    const glm::vec4 local = glm::inverse(model) * glm::vec4(world, 1.0f);
    if (!std::isfinite(local.w) || std::abs(local.w) <= 1e-8f) {
        return glm::vec3(0.0f);
    }
    return glm::vec3(local) / local.w;
}

glm::vec3 TerrainLocalToWorld(const glm::mat4& model, const glm::vec3& local) {
    return glm::vec3(model * glm::vec4(local, 1.0f));
}

// 局部 XZ 处的局部高度（双线性采样 + 反归一化）。
// CPU 镜像是顶左原点的 PNG 行序，而顶点着色器采样的是自底向上上传后的纹理：
// uv.y = 0 落在 PNG 最后一行，所以 v 与行号方向相反。
float SampleLocalTerrainHeight(const HeightmapSampling& map, float localX, float localZ) {
    if (map.samples == nullptr || map.width == 0 || map.height == 0) {
        return map.heightOffset;
    }
    const float u = std::clamp(localX / map.worldSize.x + 0.5f, 0.0f, 1.0f);
    const float v = std::clamp(localZ / map.worldSize.y + 0.5f, 0.0f, 1.0f);
    const float texelX = u * static_cast<float>(map.width - 1);
    const float texelY = (1.0f - v) * static_cast<float>(map.height - 1);

    const int maxX = static_cast<int>(map.width) - 1;
    const int maxY = static_cast<int>(map.height) - 1;
    const int x0 = std::clamp(static_cast<int>(std::floor(texelX)), 0, maxX);
    const int y0 = std::clamp(static_cast<int>(std::floor(texelY)), 0, maxY);
    const int x1 = std::min(x0 + 1, maxX);
    const int y1 = std::min(y0 + 1, maxY);
    const float fx = texelX - static_cast<float>(x0);
    const float fy = texelY - static_cast<float>(y0);

    const auto Sample = [&map](int x, int y) {
        const size_t index = static_cast<size_t>(y) * map.width + static_cast<size_t>(x);
        return static_cast<float>(map.samples[index]) * (1.0f / 65535.0f);
    };
    const float top = glm::mix(Sample(x0, y0), Sample(x1, y0), fx);
    const float bottom = glm::mix(Sample(x0, y1), Sample(x1, y1), fx);
    const float normalized = glm::mix(top, bottom, fy);
    return map.heightOffset + normalized * map.heightScale;
}

bool IntersectLocalAABB(const glm::vec3& origin, const glm::vec3& direction,
                        const glm::vec3& minBound, const glm::vec3& maxBound,
                        float& outNear, float& outFar) {
    outNear = -std::numeric_limits<float>::infinity();
    outFar = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) <= 1e-8f) {
            if (origin[axis] < minBound[axis] || origin[axis] > maxBound[axis]) {
                return false;
            }
            continue;
        }
        const float inverse = 1.0f / direction[axis];
        float nearT = (minBound[axis] - origin[axis]) * inverse;
        float farT = (maxBound[axis] - origin[axis]) * inverse;
        if (nearT > farT) {
            std::swap(nearT, farT);
        }
        outNear = std::max(outNear, nearT);
        outFar = std::min(outFar, farT);
        if (outNear > outFar) {
            return false;
        }
    }
    return outFar >= 0.0f;
}

} // namespace

bool TerrainRenderer::GetHeightmapInfo(ECS::Entity entity, uint32_t& outWidth, uint32_t& outHeight,
                                       bool& outProcedural) const {
    const auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    outWidth = it->second->heightmapWidth;
    outHeight = it->second->heightmapHeight;
    outProcedural = it->second->heightmapProcedural;
    return true;
}

bool TerrainRenderer::SampleTerrainWorldHeight(ECS::Entity entity, float worldX, float worldZ,
                                               float& outWorldY) const {
    const auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    const Resource& resource = *it->second;
    if (resource.heightmapCpu.empty()) {
        return false;
    }

    const glm::vec3 local = WorldToTerrainLocal(resource.model, glm::vec3(worldX, 0.0f, worldZ));
    const HeightmapSampling map{resource.heightmapCpu.data(), resource.heightmapWidth,
                                resource.heightmapHeight, resource.settings.worldSize,
                                resource.settings.heightScale, resource.settings.heightOffset};
    const float localY = SampleLocalTerrainHeight(map, local.x, local.z);
    outWorldY = TerrainLocalToWorld(resource.model, glm::vec3(local.x, localY, local.z)).y;
    return true;
}

bool TerrainRenderer::RaycastTerrainWorld(ECS::Entity entity,
                                          const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                                          glm::vec3& outWorldHit) const {
    const auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    const Resource& resource = *it->second;
    if (resource.heightmapCpu.empty()) {
        return false;
    }

    const glm::mat4 inverseModel = glm::inverse(resource.model);
    const glm::vec3 localOrigin = glm::vec3(inverseModel * glm::vec4(rayOrigin, 1.0f));
    const glm::vec3 localDirection = glm::vec3(inverseModel * glm::vec4(rayDirection, 0.0f));
    if (!std::isfinite(glm::length(localDirection)) || glm::length(localDirection) <= 1e-8f) {
        return false;
    }

    const HeightmapSampling map{resource.heightmapCpu.data(), resource.heightmapWidth,
                                resource.heightmapHeight, resource.settings.worldSize,
                                resource.settings.heightScale, resource.settings.heightOffset};

    const float halfX = resource.settings.worldSize.x * 0.5f;
    const float halfZ = resource.settings.worldSize.y * 0.5f;
    const float height0 = resource.settings.heightOffset;
    const float height1 = resource.settings.heightOffset + resource.settings.heightScale;
    const glm::vec3 minBound(-halfX, std::min(height0, height1), -halfZ);
    const glm::vec3 maxBound(halfX, std::max(height0, height1), halfZ);

    float nearT = 0.0f;
    float farT = 0.0f;
    if (!IntersectLocalAABB(localOrigin, localDirection, minBound, maxBound, nearT, farT)) {
        return false;
    }
    nearT = std::max(nearT, 0.0f);
    if (farT <= nearT) {
        return false;
    }

    // 步进找"射线上方 → 射线下方"的符号翻转，再二分细化到约 1/4096 跨度。
    constexpr int kSteps = 384;
    constexpr int kRefineIterations = 12;
    const float span = farT - nearT;
    const float step = span / static_cast<float>(kSteps);

    bool found = false;
    float hitT = 0.0f;
    float previousT = nearT;
    float previousDelta = 0.0f;
    for (int i = 0; i <= kSteps; ++i) {
        const float t = nearT + step * static_cast<float>(i);
        const glm::vec3 point = localOrigin + localDirection * t;
        const float delta = point.y - SampleLocalTerrainHeight(map, point.x, point.z);
        if (i > 0 && previousDelta > 0.0f && delta <= 0.0f) {
            float low = previousT;
            float high = t;
            for (int iteration = 0; iteration < kRefineIterations; ++iteration) {
                const float middle = 0.5f * (low + high);
                const glm::vec3 probe = localOrigin + localDirection * middle;
                if (probe.y - SampleLocalTerrainHeight(map, probe.x, probe.z) > 0.0f) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            hitT = 0.5f * (low + high);
            found = true;
            break;
        }
        previousT = t;
        previousDelta = delta;
    }
    if (!found) {
        return false;
    }

    const glm::vec3 localHit = localOrigin + localDirection * hitT;
    outWorldHit = TerrainLocalToWorld(resource.model, localHit);
    return true;
}

bool TerrainRenderer::SculptTerrainWorld(ECS::Entity entity, float worldX, float worldZ,
                                         float radius, float normalizedDelta) {
    if (radius <= 0.0f || normalizedDelta == 0.0f) {
        return false;
    }

    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    Resource& resource = *it->second;
    if (resource.heightmapCpu.empty() || resource.heightmapWidth < 2 || resource.heightmapHeight < 2) {
        return false;
    }

    const glm::vec2 worldSize = resource.settings.worldSize;
    const glm::vec3 local = WorldToTerrainLocal(resource.model, glm::vec3(worldX, 0.0f, worldZ));

    const float maxTexelX = static_cast<float>(resource.heightmapWidth - 1);
    const float maxTexelY = static_cast<float>(resource.heightmapHeight - 1);
    const float texelsPerWorldX = maxTexelX / std::max(worldSize.x, 1e-4f);
    const float texelsPerWorldZ = maxTexelY / std::max(worldSize.y, 1e-4f);

    // 笔刷中心 → texel（顶左原点行序）
    const float centerTexelX = std::clamp(local.x / worldSize.x + 0.5f, 0.0f, 1.0f) * maxTexelX;
    const float centerTexelY = (1.0f - std::clamp(local.z / worldSize.y + 0.5f, 0.0f, 1.0f)) * maxTexelY;

    const float radiusTexelX = radius * texelsPerWorldX;
    const float radiusTexelY = radius * texelsPerWorldZ;

    const int minX = std::max(0, static_cast<int>(std::floor(centerTexelX - radiusTexelX)));
    const int maxX = std::min(static_cast<int>(resource.heightmapWidth) - 1,
                              static_cast<int>(std::ceil(centerTexelX + radiusTexelX)));
    const int minY = std::max(0, static_cast<int>(std::floor(centerTexelY - radiusTexelY)));
    const int maxY = std::min(static_cast<int>(resource.heightmapHeight) - 1,
                              static_cast<int>(std::ceil(centerTexelY + radiusTexelY)));
    if (minX > maxX || minY > maxY) {
        return false;
    }

    const float deltaSamples = normalizedDelta * 65535.0f;
    const float inverseRadius = 1.0f / std::max(radius, 1e-4f);
    bool changed = false;

    for (int y = minY; y <= maxY; ++y) {
        // 衰减在局部世界空间度量：非正方形世界尺寸 / 非正方高度图下笔刷仍是正圆。
        const float v = 1.0f - static_cast<float>(y) / maxTexelY;
        const float localZ = (v - 0.5f) * worldSize.y;
        for (int x = minX; x <= maxX; ++x) {
            const float u = static_cast<float>(x) / maxTexelX;
            const float localX = (u - 0.5f) * worldSize.x;

            const float offsetX = localX - local.x;
            const float offsetZ = localZ - local.z;
            const float distance = std::sqrt(offsetX * offsetX + offsetZ * offsetZ);
            if (distance > radius) {
                continue;
            }

            // smoothstep 衰减：中心权重 1、边缘收敛到 0 且一阶连续，
            // 这样笔刷轨迹不会留下"突然升高/降低"的硬边或者台阶。
            const float falloffT = 1.0f - distance * inverseRadius;
            const float falloff = falloffT * falloffT * (3.0f - 2.0f * falloffT);

            const size_t index = static_cast<size_t>(y) * resource.heightmapWidth +
                                 static_cast<size_t>(x);
            const float updated = static_cast<float>(resource.heightmapCpu[index]) +
                                  deltaSamples * falloff;
            const uint16_t clamped = static_cast<uint16_t>(std::clamp(updated, 0.0f, 65535.0f));
            if (clamped != resource.heightmapCpu[index]) {
                resource.heightmapCpu[index] = clamped;
                changed = true;
            }
        }
    }

    if (!changed) {
        return false;
    }

    if (g_TexturePool) {
        // 上传失败必须响亮报错：CPU 镜像已改而 GPU 没跟上，画面会静默地不更新。
        const bool uploaded = g_TexturePool->UpdateHeightmapRegion16(
            resource.heightmapKey,
            static_cast<uint32_t>(minX), static_cast<uint32_t>(minY),
            static_cast<uint32_t>(maxX - minX + 1), static_cast<uint32_t>(maxY - minY + 1),
            resource.heightmapCpu.data(), resource.heightmapWidth);
        if (!uploaded) {
            LOGE("[TerrainRenderer] heightmap brush: heightmap region upload failed "
                 "(entity=%u key=%s rect=%d,%d %dx%d)",
                 static_cast<unsigned>(entity), resource.heightmapKey.c_str(),
                 minX, minY, maxX - minX + 1, maxY - minY + 1);
            return false;
        }
    }
    return true;
}

bool TerrainRenderer::GetControlMapInfo(ECS::Entity entity, uint32_t& outWidth, uint32_t& outHeight,
                                        bool& outProcedural, bool& outPaintable) const {
    const auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    outWidth = it->second->controlWidth;
    outHeight = it->second->controlHeight;
    outProcedural = it->second->controlProcedural;
    outPaintable = !it->second->controlCpu.empty();
    return true;
}

bool TerrainRenderer::PaintTerrainMaterialWorld(ECS::Entity entity, float worldX, float worldZ,
                                                float radius, int layerIndex,
                                                float hardness, float amount) {
    if (radius <= 0.0f || amount <= 0.0f || layerIndex < 0 || layerIndex > 3) {
        return false;
    }

    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    Resource& resource = *it->second;
    if (resource.controlCpu.empty() || resource.controlWidth < 2 || resource.controlHeight < 2) {
        return false;
    }

    const glm::vec2 worldSize = resource.settings.worldSize;
    const glm::vec3 local = WorldToTerrainLocal(resource.model, glm::vec3(worldX, 0.0f, worldZ));

    const float maxTexelX = static_cast<float>(resource.controlWidth - 1);
    const float maxTexelY = static_cast<float>(resource.controlHeight - 1);
    const float texelsPerWorldX = maxTexelX / std::max(worldSize.x, 1e-4f);
    const float texelsPerWorldZ = maxTexelY / std::max(worldSize.y, 1e-4f);

    // 笔刷中心 → texel（控制图与高度图同为顶左原点行序）
    const float centerTexelX = std::clamp(local.x / worldSize.x + 0.5f, 0.0f, 1.0f) * maxTexelX;
    const float centerTexelY = (1.0f - std::clamp(local.z / worldSize.y + 0.5f, 0.0f, 1.0f)) * maxTexelY;

    const float radiusTexelX = radius * texelsPerWorldX;
    const float radiusTexelY = radius * texelsPerWorldZ;

    const int minX = std::max(0, static_cast<int>(std::floor(centerTexelX - radiusTexelX)));
    const int maxX = std::min(static_cast<int>(resource.controlWidth) - 1,
                              static_cast<int>(std::ceil(centerTexelX + radiusTexelX)));
    const int minY = std::max(0, static_cast<int>(std::floor(centerTexelY - radiusTexelY)));
    const int maxY = std::min(static_cast<int>(resource.controlHeight) - 1,
                              static_cast<int>(std::ceil(centerTexelY + radiusTexelY)));
    if (minX > maxX || minY > maxY) {
        return false;
    }

    // 软硬的唯一旋钮是"平顶核心半径"：核心内权重恒为 1，核心到半径之间用 smoothstep
    // 过渡。hardness=1 → 核心几乎等于半径（只剩外缘一条窄带做渐变，视觉上是硬边）；
    // hardness=0 → 核心为 0（从圆心到边缘全程渐变，视觉上最软）。
    // 过渡带下界取 1.5 texel：最硬档若让过渡带窄于一个 texel，边界会退化成按 texel
    // 硬切，在高度图/控制图分辨率不够时会看到明显锯齿。
    const float texelWorld = std::min(std::abs(worldSize.x) / maxTexelX,
                                      std::abs(worldSize.y) / maxTexelY);
    const float band = std::max(radius * (1.0f - std::clamp(hardness, 0.0f, 1.0f)),
                                std::max(texelWorld * 1.5f, 1e-4f));
    const float coreRadius = std::max(radius - band, 0.0f);

    const float inverseBand = 1.0f / band;
    const float paintedWeight = std::clamp(amount, 0.0f, 1.0f);
    bool changed = false;

    for (int y = minY; y <= maxY; ++y) {
        // 与高度笔刷一致：衰减在局部世界空间度量，非正方形世界尺寸下笔刷仍是正圆。
        const float v = 1.0f - static_cast<float>(y) / maxTexelY;
        const float localZ = (v - 0.5f) * worldSize.y;
        for (int x = minX; x <= maxX; ++x) {
            const float u = static_cast<float>(x) / maxTexelX;
            const float localX = (u - 0.5f) * worldSize.x;

            const float offsetX = localX - local.x;
            const float offsetZ = localZ - local.z;
            const float distance = std::sqrt(offsetX * offsetX + offsetZ * offsetZ);
            if (distance > radius) {
                continue;
            }

            float profile = 1.0f;
            if (distance > coreRadius) {
                const float t = std::clamp((radius - distance) * inverseBand, 0.0f, 1.0f);
                profile = t * t * (3.0f - 2.0f * t);
            }

            const float blend = paintedWeight * profile;
            if (blend <= 0.0f) {
                continue;
            }

            uint8_t* texel = &resource.controlCpu[(static_cast<size_t>(y) * resource.controlWidth +
                                                   static_cast<size_t>(x)) * 4];
            // 目标分布是"独热"：目标层权重 1、其余层 0。着色器按四通道归一化混合，
            // 所以把整条 RGBA 一起朝独热混合，才能得到"这块地方就是这种材质"的效果，
            // 而不是把新材质叠在旧材质上各占一半。
            for (int channel = 0; channel < 4; ++channel) {
                const float target = (channel == layerIndex) ? 255.0f : 0.0f;
                const float updated = static_cast<float>(texel[channel]) +
                                      (target - static_cast<float>(texel[channel])) * blend;
                const uint8_t clamped =
                    static_cast<uint8_t>(std::lround(std::clamp(updated, 0.0f, 255.0f)));
                if (clamped != texel[channel]) {
                    texel[channel] = clamped;
                    changed = true;
                }
            }
        }
    }

    if (!changed) {
        return false;
    }

    if (g_TexturePool) {
        // 上传失败必须响亮报错：CPU 镜像已改而 GPU 没跟上，画面会静默地不更新。
        const bool uploaded = g_TexturePool->UpdateControlMapRegion8(
            resource.controlKey,
            static_cast<uint32_t>(minX), static_cast<uint32_t>(minY),
            static_cast<uint32_t>(maxX - minX + 1), static_cast<uint32_t>(maxY - minY + 1),
            resource.controlCpu.data(), resource.controlWidth);
        if (!uploaded) {
            LOGE("[TerrainRenderer] material brush: control map region upload failed "
                 "(entity=%u key=%s rect=%d,%d %dx%d layer=%d)",
                 static_cast<unsigned>(entity), resource.controlKey.c_str(),
                 minX, minY, maxX - minX + 1, maxY - minY + 1, layerIndex);
            return false;
        }
    }
    return true;
}

bool TerrainRenderer::GetGrassMapInfo(ECS::Entity entity, uint32_t& outWidth,
                                      uint32_t& outHeight, bool& outPaintable) const {
    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    const Resource& resource = *it->second;
    outWidth = resource.grassWidth;
    outHeight = resource.grassHeight;
    outPaintable = !resource.grassCpu.empty() && !resource.grassKey.empty();
    return true;
}

bool TerrainRenderer::PaintTerrainGrassWorld(ECS::Entity entity, float worldX, float worldZ,
                                             float radius, float targetDensity,
                                             float hardness, float amount) {
    if (radius <= 0.0f || amount <= 0.0f) {
        return false;
    }

    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    Resource& resource = *it->second;
    if (resource.grassCpu.empty() || resource.grassWidth < 2 || resource.grassHeight < 2 ||
        resource.grassKey.empty()) {
        return false;
    }

    const glm::vec2 worldSize = resource.settings.worldSize;
    const glm::vec3 local = WorldToTerrainLocal(resource.model, glm::vec3(worldX, 0.0f, worldZ));

    const float maxTexelX = static_cast<float>(resource.grassWidth - 1);
    const float maxTexelY = static_cast<float>(resource.grassHeight - 1);
    const float texelsPerWorldX = maxTexelX / std::max(worldSize.x, 1e-4f);
    const float texelsPerWorldZ = maxTexelY / std::max(worldSize.y, 1e-4f);

    // 与材质笔刷同一套顶左原点行序映射。
    const float centerTexelX = std::clamp(local.x / worldSize.x + 0.5f, 0.0f, 1.0f) * maxTexelX;
    const float centerTexelY = (1.0f - std::clamp(local.z / worldSize.y + 0.5f, 0.0f, 1.0f)) * maxTexelY;
    const float radiusTexelX = radius * texelsPerWorldX;
    const float radiusTexelY = radius * texelsPerWorldZ;

    const int minX = std::max(0, static_cast<int>(std::floor(centerTexelX - radiusTexelX)));
    const int maxX = std::min(static_cast<int>(resource.grassWidth) - 1,
                              static_cast<int>(std::ceil(centerTexelX + radiusTexelX)));
    const int minY = std::max(0, static_cast<int>(std::floor(centerTexelY - radiusTexelY)));
    const int maxY = std::min(static_cast<int>(resource.grassHeight) - 1,
                              static_cast<int>(std::ceil(centerTexelY + radiusTexelY)));
    if (minX > maxX || minY > maxY) {
        return false;
    }

    // 硬度语义与材质笔刷完全一致：平顶核心 + smoothstep 过渡带（下限 1.5 texel 防锯齿）。
    const float texelWorld = std::min(std::abs(worldSize.x) / maxTexelX,
                                      std::abs(worldSize.y) / maxTexelY);
    const float band = std::max(radius * (1.0f - std::clamp(hardness, 0.0f, 1.0f)),
                                std::max(texelWorld * 1.5f, 1e-4f));
    const float coreRadius = std::max(radius - band, 0.0f);

    const float inverseBand = 1.0f / band;
    const float paintedAmount = std::clamp(amount, 0.0f, 1.0f);
    const float targetValue = std::clamp(targetDensity, 0.0f, 1.0f) * 255.0f;
    bool changed = false;

    for (int y = minY; y <= maxY; ++y) {
        const float v = 1.0f - static_cast<float>(y) / maxTexelY;
        const float localZ = (v - 0.5f) * worldSize.y;
        for (int x = minX; x <= maxX; ++x) {
            const float u = static_cast<float>(x) / maxTexelX;
            const float localX = (u - 0.5f) * worldSize.x;

            const float offsetX = localX - local.x;
            const float offsetZ = localZ - local.z;
            const float distance = std::sqrt(offsetX * offsetX + offsetZ * offsetZ);
            if (distance > radius) {
                continue;
            }

            float profile = 1.0f;
            if (distance > coreRadius) {
                const float t = std::clamp((radius - distance) * inverseBand, 0.0f, 1.0f);
                profile = t * t * (3.0f - 2.0f * t);
            }
            const float blend = paintedAmount * profile;

            uint8_t& texel = resource.grassCpu[static_cast<size_t>(y) * resource.grassWidth +
                                                static_cast<size_t>(x)];
            const float updated = static_cast<float>(texel) + (targetValue - static_cast<float>(texel)) * blend;
            const uint8_t clamped = static_cast<uint8_t>(std::lround(std::clamp(updated, 0.0f, 255.0f)));
            if (clamped != texel) {
                texel = clamped;
                changed = true;
            }
        }
    }

    if (!changed) {
        return false;
    }

    if (g_TexturePool) {
        const bool uploaded = g_TexturePool->UpdateGrassMaskRegion8(
            resource.grassKey,
            static_cast<uint32_t>(minX), static_cast<uint32_t>(minY),
            static_cast<uint32_t>(maxX - minX + 1), static_cast<uint32_t>(maxY - minY + 1),
            resource.grassCpu.data(), resource.grassWidth);
        if (!uploaded) {
            LOGE("[TerrainRenderer] grass brush: grass mask region upload failed "
                 "(entity=%u key=%s rect=%d,%d %dx%d)",
                 static_cast<unsigned>(entity), resource.grassKey.c_str(),
                 minX, minY, maxX - minX + 1, maxY - minY + 1);
            return false;
        }
    }
    // 镜像已变，下一帧主 pass 重建散布实例。
    resource.grassDirty = true;
    return true;
}
