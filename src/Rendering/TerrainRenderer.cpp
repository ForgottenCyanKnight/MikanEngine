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

namespace {

constexpr size_t kInitialDescriptorSets = 512;

// ===== 草地 GPU 逐桶剔除（阶段一）的 GPU 端结构 =====
// 与 grass_cull.comp 严格一致；push constant 共 128 字节。
struct GpuGrassBucket {
    glm::vec4 minFirst;   // xyz = 世界空间 AABB min，w = firstInstance
    glm::vec4 maxCount;   // xyz = 世界空间 AABB max，w = instanceCount
};
static_assert(sizeof(GpuGrassBucket) == 32, "gpu grass bucket must be 32 bytes");

struct GrassCullPush {
    glm::vec4 planes[6];        // xyz = normal, w = distance（与 AABB::IsInsideFrustum 同约定）
    glm::vec4 cameraPosDist;    // xyz = 相机位置, w = 草可见距离（米）
    glm::uvec4 params;          // x = 桶总数, y = 视图段（命令缓冲内偏移，单位 = 桶）
};
static_assert(sizeof(GrassCullPush) == 128, "grass cull push constant must be 128 bytes");

// 程序化平坦高度图：heightmapPath 为空时地形默认是一张平面。
// 512 在 256 世界单位下约 0.5 单位/texel，对编辑器笔刷粒度足够。
constexpr uint32_t kProceduralHeightmapResolution = 512;
// 平坦基准取归一化中值，升高/降低两个方向都留有余量；
// 配合预设里的 heightOffset = -heightScale/2，平坦面正好落在局部 y = 0。
constexpr uint16_t kProceduralFlatSample = 32768;

// 草可见距离（米）：超出后顶点着色器把叶片收缩到相机外。草叶高约 1.02m，
// 100m 处在 1080p 下不足 2 像素——更远的草只有像素级 overdraw 没有信息量，
// 直接不画。密度衰减（45% 视距起 hash 逐株抽稀）+ 视距内整体溶解都在
// grass.vert 里做，主 pass 与阴影 pass 严格一致。
constexpr float kGrassViewDistance = 100.0f;
// 草地段数分级 LOD 总开关（MIKAN_GRASS_LOD=0 关闭；默认启用）。经 push
// constant csmParams.y 传给 grass.vert：近景 4 段 / 中景 2 段 / 远景 1 段，
// 主 pass 与 CSM 阴影共用同一开关，保证投影的草与渲染的草逐叶一致。
const bool kGrassSegmentLodEnabled = []{
    const char* env = std::getenv("MIKAN_GRASS_LOD");
    return !(env != nullptr && env[0] == '0');
}();
// 叶片级剔除的叶级保守半径（XZ 方向）：叶高 0.42-1.02m + 风摆余量 + 地形局部
// 起伏。Y 方向由 FinalizeGrassBuckets 扫高度镜像的全局 [minY, maxY] 兜底。
constexpr float kGrassBladeCullRadius = 1.5f;
// CPU 粗筛桶列表容量上限（65536 × 16B = 1MB hostVisible）：覆盖 subdiv=8
// 的 16384 桶全可见情形。上传按 64KB 分段（vkCmdUpdateBuffer 单次数据上限）。
// 超上限回退全量 dispatch 模式（shader info.z=1）。
constexpr uint32_t kGrassBladeBucketListCap = 65536;
// vkCmdUpdateBuffer 单次调用最大条目数（2048 × 32B = 64KB）。
constexpr uint32_t kGrassBladeBucketUploadChunk = 2048;

// g_PhysicalDevice 由 Core/VulkanContext.h 声明（本文件已包含）。

// params SSBO 与 grass_blade_cull.comp 的 ParamsBuf（std430）逐字段对齐：
// mat4=64B + vec4[6]=96B + vec4=16B + vec4=16B = 192B。
struct GrassBladeCullParams {
    glm::mat4 model;
    glm::vec4 planes[6];
    glm::vec4 camAndDist;
    glm::vec4 heightRange;
};
static_assert(sizeof(GrassBladeCullParams) == 192,
              "GrassBladeCullParams must match grass_blade_cull.comp std430 layout (192B)");

// 叶片级 dispatch 的 push constant：x = viewSlot（命令/紧凑流/参数段序号），
// y = 源实例数。与 grass_blade_cull.comp 的 PC 块逐字段一致。
struct GrassBladeCullPush {
    glm::uvec4 info;
};
static_assert(sizeof(GrassBladeCullPush) == 16,
              "GrassBladeCullPush must match grass_blade_cull.comp push constant (16B)");
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
    m_WaterPipeline.Cleanup();
    m_CsmRenderPass = VK_NULL_HANDLE;
    CleanupGrassCullResources();
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
        // 草剔除参考系缓存：只在"带视锥"的 Prepare 时更新。阴影 pass 的
        // noTerrainFrustum Prepare（useFrustumCulling=false）不得清掉主几何
        // 阶段写入的参考系，否则主 pass 剔除会退化成当前渲染视图平面——在
        // 编辑器"主相机剔除"模式下与地形 chunk 参考系分裂（游戏视锥外的草
        // 照画而地形已剔，2026-09-18 用户截图报告的问题）。
        if (useFrustumCulling) {
            resource->grassFrustumPlanes = frustumPlanes;
            resource->grassUseFrustumCulling = true;
            resource->grassCameraPosition = cameraPosition;
        }
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
        // 草剔除参考系缓存：只在"带视锥"的 Prepare 时更新。阴影 pass 的
        // noTerrainFrustum Prepare（useFrustumCulling=false）不得清掉主几何
        // 阶段写入的参考系，否则主 pass 剔除会退化成当前渲染视图平面——在
        // 编辑器"主相机剔除"模式下与地形 chunk 参考系分裂（游戏视锥外的草
        // 照画而地形已剔，2026-09-18 用户截图报告的问题）。
        if (useFrustumCulling) {
            resource->grassFrustumPlanes = frustumPlanes;
            resource->grassUseFrustumCulling = true;
            resource->grassCameraPosition = cameraPosition;
        }
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
    if (m_WaterPipeline.GetPipeline() != VK_NULL_HANDLE) {
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
        bladeSets.reserve(resource.grassBladeCullDescriptorSets.size());
        for (VkDescriptorSet& set : resource.grassBladeCullDescriptorSets) {
            if (set != VK_NULL_HANDLE) {
                bladeSets.push_back(set);
                set = VK_NULL_HANDLE;
            }
        }
        if (!bladeSets.empty()) {
            vkFreeDescriptorSets(g_Device, m_GrassBladeCullDescriptorPool,
                                 static_cast<uint32_t>(bladeSets.size()), bladeSets.data());
        }
    }
    for (auto& patch : resource.patches) {
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
    poolSizes[0].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets);
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(kInitialDescriptorSets * 7);

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

    // 水面网格管线：覆盖整块地形的静态 UV 网格（无实例，顶点 = vec2 UV），
    // terrain_water.vert 按 高度图+水位图 抬升顶点。与地形同 G-buffer subpass、
    // 同描述符布局；不写 CSM（水面不投影）。失败只降级"无水面"。
    {
        const std::array<VkVertexInputBindingDescription, 1> waterBindings = {
            MakeVertexBinding(0, sizeof(glm::vec2), VK_VERTEX_INPUT_RATE_VERTEX)
        };
        const std::array<VkVertexInputAttributeDescription, 1> waterAttributes = {
            MakeVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0)
        };
        PipelineConfig waterConfig;
        waterConfig.vertShader = "terrain_water.vert.spv";
        waterConfig.fragShader = "terrain_water.frag.spv";
        // 复用 BuildPatch 的逐行 strip + primitive restart 网格。
        waterConfig.topology = m_PrimitiveRestartSupported
            ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        waterConfig.primitiveRestartEnable = m_PrimitiveRestartSupported;
        // 网格顶点绕向依赖高度图抬升方向，直接关剔除（多耗可忽略：
        // 干燥区域三角形在深度测试阶段即被地形剔除）。
        waterConfig.cullMode = VK_CULL_MODE_NONE;
        waterConfig.depthTest = true;
        waterConfig.depthWrite = true;
        waterConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        waterConfig.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
        waterConfig.colorWriteMasks = geometryConfig.colorWriteMasks;
        waterConfig.subpass = 1;
        waterConfig.vertexBindings.assign(waterBindings.begin(), waterBindings.end());
        waterConfig.vertexAttributes.assign(waterAttributes.begin(), waterAttributes.end());
        if (!m_WaterPipeline.Create(m_RenderPass, m_DescriptorLayout, waterConfig)) {
            LOGE("[TerrainRenderer] water surface pipeline creation failed - water surface disabled");
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

        std::array<VkWriteDescriptorSet, 8> writes{};
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
                                       kTerrainWaterMaxDepth);
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
    // 水下不长草：水位深度超过 1/32 满深（约 0.25m）的 texel 不计不种。
    const bool hasWaterMap = resource.waterCpu.size() == resource.grassCpu.size();
    const uint8_t kGrassWaterCutoff = 8;
    uint64_t grassDemand = 0;
    for (uint32_t y = 0; y < grassHeight; ++y) {
        for (uint32_t x = 0; x < grassWidth; ++x) {
            const uint8_t density = resource.grassCpu[static_cast<size_t>(y) * grassWidth + x];
            if (density == 0) {
                continue;
            }
            if (hasWaterMap &&
                resource.waterCpu[static_cast<size_t>(y) * grassWidth + x] > kGrassWaterCutoff) {
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
            if (hasWaterMap &&
                resource.waterCpu[static_cast<size_t>(y) * grassWidth + x] > kGrassWaterCutoff) {
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
                                            0.42f + randB * 0.6f);           // height (m)
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
    // 桶流重建后 GPU 端世界空间桶 SSBO 需要重传（含模型矩阵变换后的 AABB）。
    resource.grassGpuBucketDirty = true;
    if (resource.grassStaging.empty()) {
        // 无草时兜底：用高度偏移/缩放的理论范围（叶片级剔除的 Y 区间参数）
        resource.grassBladeMinY = std::min(resource.settings.heightOffset,
                                           resource.settings.heightOffset + resource.settings.heightScale) - 0.5f;
        resource.grassBladeMaxY = std::max(resource.settings.heightOffset,
                                           resource.settings.heightOffset + resource.settings.heightScale) + 2.0f;
        return;
    }

    // 区块内细分：1 = 与地形 chunk 同粒度；2 = 每区块 2×2 子格（1/4 大小）。
    // 可用 MIKAN_GRASS_BUCKET_SUBDIV 调大（1-64）：GPU/compute 剔除下桶数
    // 增多几乎零成本（剔除是每桶一次 AABB 测试，CPU 只承担一次计数排序分桶），
    // 桶越细 AABB 越紧、视锥/距离剔除越狠——CPU 逐桶剔除时代为控制 CPU 开销
    // 只能取 2，GPU 剔除时代建议 8+。
    static const int kGrassBucketSubdiv = []{
        const char* env = std::getenv("MIKAN_GRASS_BUCKET_SUBDIV");
        int v = env ? std::atoi(env) : 2;
        return std::clamp(v, 1, 64);
    }();
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
    constexpr float kLeafHeightMargin = 2.0f;   // 叶高 1.02m + 风摆/增益余量
    constexpr float kGroundMargin = 0.5f;       // 根部贴地，向下只留采样余量

    resource.grassBuckets.resize(bucketTotal);
    std::vector<uint32_t> writeCursor(bucketTotal, 0);
    uint32_t running = 0;
    // 叶片级剔除的全局 Y 范围：所有桶 yLow/yHigh 的 min/max（叶片级 compute
    // 不知道每叶精确根部 Y，用该区间作保守 AABB 的 Y 项，随雕刻自动更新）。
    float globalMinY = std::numeric_limits<float>::max();
    float globalMaxY = std::numeric_limits<float>::lowest();
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
        globalMinY = std::min(globalMinY, yLow);
        globalMaxY = std::max(globalMaxY, yHigh);
    }
    resource.grassBladeMinY = globalMinY;
    resource.grassBladeMaxY = globalMaxY;

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
                    // STORAGE_BUFFER_BIT：叶片级剔除 compute 以 SSBO 读源实例流。
                    created = created && buffer.Create(
                        static_cast<VkDeviceSize>(newCapacity * sizeof(GrassBladeInstance)),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        memory);
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
                                  uint32_t frame, const glm::mat4& projView) {
    if (m_GrassPipeline.GetPipeline() == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t instanceCount = EnsureGrassInstancesUploaded(resource, frame);
    if (instanceCount == 0) {
        return;
    }
    static const bool statsEnabled = []{
        const char* env = std::getenv("MIKAN_GRASS_CULL_STATS");
        return env != nullptr && env[0] == '1';
    }();

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_GrassPipeline.GetPipeline());
    // 主 pass 模式：useCsm=0，grass.vert 用 UBO 的 projView（含 TAA 抖动）。
    {
        struct GrassPushData {
            glm::mat4 csmProjView;
            glm::vec4 csmParams;
        } pushData{glm::mat4(1.0f), glm::vec4(0.0f,
                   kGrassSegmentLodEnabled ? 1.0f : 0.0f, 0.0f, 0.0f)};
        vkCmdPushConstants(commandBuffer, m_GrassPipeline.GetLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GrassPushData), &pushData);
    }
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_GrassPipeline.GetLayout(), 0, 1,
                            &resource.descriptorSets[frame], 0, nullptr);
    VkBuffer instanceBuffer = resource.grassInstanceBuffers[frame].GetBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &instanceBuffer, &offset);

    // ===== 剔除参考系解析（叶片级 / GPU 桶级 / CPU 回退三分支共用）=====
    // 优先复用 Prepare 缓存（与地形 chunk 严格同源——编辑器场景视图开启"主相机
    // 剔除"时，Prepare 传的就是主相机视锥，草必须用同一套平面，否则场景视图里
    // 游戏视锥外的草照画而地形已剔）。缓存无效（首帧/尚未有带视锥的 Prepare）
    // 时用当前视图 projView 现场提取平面兜底，相机位置从 projView 逆矩阵提取，
    // 与剔除平面严格同源。
    glm::mat4 renderProjView = projView;
    static const bool probe = []{
        const char* env = std::getenv("MIKAN_GRASS_CULL_PROBE");
        return env != nullptr && env[0] == '1';
    }();
    bool probeActive = false;
    if (probe) {
        static int s_fallbackProbeCall = 0;
        ++s_fallbackProbeCall;
        if (s_fallbackProbeCall == 16) {
            LOGI("[Probe] grass cull probe: frustum -300m Y from call %d",
                 s_fallbackProbeCall);
        }
        if (s_fallbackProbeCall >= 16) {
            renderProjView[3].y += 300.0f;
            probeActive = true; // 探针强制走现场平面分支，保持剔除数学可验证
        }
    }
    const glm::vec3 fallbackCam(glm::inverse(renderProjView)[3]);
    const std::array<Plane, 6> fallbackPlanes = AABBUtils::ExtractFrustumPlanes(renderProjView);
    const bool useCachedCull = resource.grassUseFrustumCulling && !probeActive;
    const std::array<Plane, 6>& cullPlanes = useCachedCull
        ? resource.grassFrustumPlanes : fallbackPlanes;
    const glm::vec3& cullCam = useCachedCull
        ? resource.grassCameraPosition : fallbackCam;

    if (statsEnabled && useCachedCull) {
        // 一致性自检：单视图（headless/游戏视图）下缓存平面与当前视图现场
        // 平面应给出相同的可见桶数——不等说明参考系传递有 bug。
        uint32_t cachedVisible = 0, liveVisible = 0;
        for (const GrassChunkBucket& bucket : resource.grassBuckets) {
            if (bucket.count == 0) continue;
            const AABB worldBounds = bucket.localBounds.Transform(resource.model);
            if (worldBounds.IsInsideFrustum(cullPlanes)) ++cachedVisible;
            if (worldBounds.IsInsideFrustum(fallbackPlanes)) ++liveVisible;
        }
        LOGI("[TerrainRenderer][GrassCullStats] main ref-check: cachedVisible=%u liveVisible=%u%s",
             cachedVisible, liveVisible,
             cachedVisible == liveVisible ? "" : "  <<< MISMATCH");
    }

    // ===== 叶片级 GPU 剔除（阶段三）：compute 逐叶测试 + atomicAdd 压缩实例流
    // + 一条 vkCmdDrawIndirect（instanceCount = 原子计数）。粒度 = 单叶。
    // dispatch 在 RecordGrassBladeCull（render pass 外、CSM 之前）完成，这里只
    // 按 projView 位级匹配选出本视图的段，绑定紧凑实例流并间接绘制——未命中
    // （首帧 / 该视图本帧未 dispatch）落到下方 CPU 逐桶回退。
    if (m_GrassBladeCullEnabled &&
        resource.grassBladeCmdBuffers[frame].GetBuffer() != VK_NULL_HANDLE &&
        resource.grassBladeCompactCapacity > 0) {
        for (int slot = 0; slot < kGrassCullViewSlots; ++slot) {
            const auto& cullView = resource.grassGpuCullViews[frame][slot];
            if (!cullView.dispatched || cullView.viewProj != projView) {
                continue;
            }
            // 紧凑流段偏移与命令段偏移必须与 compute 写入侧一致
            // （segment = viewSlot，容量 = 源实例数）。
            VkBuffer compactBuf = resource.grassBladeCompactBuffers[frame].GetBuffer();
            VkDeviceSize compactOff = static_cast<VkDeviceSize>(slot) *
                                      static_cast<VkDeviceSize>(resource.grassBladeCompactCapacity) *
                                      sizeof(GrassBladeInstance);
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &compactBuf, &compactOff);
            vkCmdDrawIndirect(commandBuffer, resource.grassBladeCmdBuffers[frame].GetBuffer(),
                              static_cast<VkDeviceSize>(slot) * sizeof(VkDrawIndirectCommand),
                              1, sizeof(VkDrawIndirectCommand));
            // 读回调试（MIKAN_GRASS_COMPUTE_DEBUG=1）：上一周期同槽位命令段
            // 快照（3 帧前同槽提交已完成），instanceCount = 该周期可见叶数。
            if (std::getenv("MIKAN_GRASS_COMPUTE_DEBUG") &&
                resource.grassBladeCmdBuffers[frame].GetMappedPtr() == nullptr) {
                resource.grassBladeCmdBuffers[frame].Map();
            }
            if (std::getenv("MIKAN_GRASS_COMPUTE_DEBUG")) {
                if (const uint32_t* raw = static_cast<const uint32_t*>(
                        resource.grassBladeCmdBuffers[frame].GetMappedPtr())) {
                    LOGI("[TerrainRenderer][GrassBladeDbg] readback frame=%u buf=%p slot=%d: "
                         "raw=[%u %u %u %u]", frame,
                         (void*)resource.grassBladeCmdBuffers[frame].GetBuffer(), slot,
                         raw[slot * 4 + 0], raw[slot * 4 + 1],
                         raw[slot * 4 + 2], raw[slot * 4 + 3]);
                }
            }
            if (statsEnabled) {
                LOGI("[TerrainRenderer][GrassCullStats] main: blade-level GPU cull "
                     "(slot=%d src=%u planes=%s)", slot, instanceCount,
                     useCachedCull ? "cached" : "live");
            }
            return;
        }
    }

    // GPU 桶级路径：剔除 compute（render pass 外录制，见 RecordGrassGpuCull）已把
    // 本视图的逐桶 VkDrawIndirectCommand 写进命令 SSBO 的对应段，这里一条
    // vkCmdDrawIndirect 提交全部桶——不可见桶 instanceCount=0，驱动自然跳过。
    // 段匹配用 projView 位级比较：只有"本帧确实为本视图 dispatch 过"才走 GPU 路径，
    // 否则（CSM 不可用 / MIKAN_GRASS_GPU_CULL=0 / 首帧竞态）回退 CPU 逐桶绘制。
    if (!m_GrassGpuCullDisabled &&
        resource.grassIndirectBuffers[frame].GetBuffer() != VK_NULL_HANDLE &&
        resource.grassGpuBucketTotal > 0) {
        for (int slot = 0; slot < kGrassCullViewSlots; ++slot) {
            const auto& view = resource.grassGpuCullViews[frame][slot];
            if (view.dispatched && view.viewProj == projView) {
                if (std::getenv("MIKAN_GRASS_CULL_STATS")) {
                    LOGI("[TerrainRenderer][GrassCullStats] main: GPU indirect path (slot=%d buckets=%u)", slot, resource.grassGpuBucketTotal);
                }
                // 剔除 compute 把本视图的命令写在 [slot * bucketTotal, (slot+1) * bucketTotal)
                // 段（shader cmdIndex = viewSlot * bucketTotal + i），这里必须按同一
                // 段偏移读取——slot=0 时恰好为 0，slot≥1 读错段会拿到全零命令
                // （instanceCount=0），表现为草本体消失而草影（CPU 逐桶路径）正常。
                const VkDeviceSize cmdStride = sizeof(VkDrawIndirectCommand);
                vkCmdDrawIndirect(commandBuffer,
                                  resource.grassIndirectBuffers[frame].GetBuffer(),
                                  /*offset=*/static_cast<VkDeviceSize>(slot) *
                                      static_cast<VkDeviceSize>(resource.grassGpuBucketTotal) *
                                      cmdStride,
                                  resource.grassGpuBucketTotal,
                                  cmdStride);
                return;
            }
        }
    }
    if (std::getenv("MIKAN_GRASS_CULL_STATS")) {
        LOGI("[TerrainRenderer][GrassCullStats] main: CPU fallback (gpuCullDisabled=%d)",
             m_GrassGpuCullDisabled ? 1 : 0);
    }

    // 10 顶点 = 4 段条带（2*(段数+1)），零顶点缓冲，几何全在顶点着色器里生成。
    // 逐桶做视锥 + 距离剔除：判据与 GPU 剔除路径完全一致。
    RenderGrassBuckets(commandBuffer, resource,
                       cullPlanes, true,
                       cullCam);
}

void TerrainRenderer::RenderGrassCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                                          const glm::mat4& shadowProjView,
                                          const glm::vec3& cameraPosition,
                                          const std::array<Plane, 6>& mainCameraFrustum) {
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
    } grassPushData{shadowProjView, glm::vec4(1.0f,
                    kGrassSegmentLodEnabled ? 1.0f : 0.0f, 0.0f, 0.0f)};

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
        // 再叠加主相机视锥门：级联光视锥比主相机视锥宽，相机背后/视野外的
        // 草不再收进草影（用户拍板：仅视锥体内收集）。
        const std::array<Plane, 6> lightPlanes = AABBUtils::ExtractFrustumPlanes(shadowProjView);
        RenderGrassBuckets(commandBuffer, *resource, lightPlanes, true, cameraPosition, "csm",
                           &mainCameraFrustum);
    }
}

void TerrainRenderer::RenderGrassBuckets(VkCommandBuffer commandBuffer, Resource& resource,
                                         const std::array<Plane, 6>& frustumPlanes,
                                         bool useFrustumCulling, const glm::vec3& cameraPosition,
                                         const char* statsTag,
                                         const std::array<Plane, 6>* extraFrustum) {
    // env 门控剔除统计（MIKAN_GRASS_CULL_STATS=1）：验证草逐桶剔除真实生效。
    static const bool statsEnabled = []{
        const char* env = std::getenv("MIKAN_GRASS_CULL_STATS");
        return env != nullptr && env[0] == '1';
    }();
    uint32_t statsDrawnBuckets = 0;
    uint32_t statsDrawnInstances = 0;

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
            // 草影专用第二道门：主相机视锥（extraFrustum 非空时）。
            if (extraFrustum != nullptr && !worldBounds.IsInsideFrustum(*extraFrustum)) {
                continue;
            }
        }
        ++statsDrawnBuckets;
        statsDrawnInstances += bucket.count;
        vkCmdDraw(commandBuffer, 10, bucket.count, 0, bucket.firstInstance);
    }
    if (statsEnabled) {
        LOGI("[TerrainRenderer][GrassCullStats] %s: buckets=%u drawn=%u instances=%u "
             "useFrustum=%d cam=(%.1f,%.1f,%.1f)",
             statsTag, static_cast<uint32_t>(resource.grassBuckets.size()),
             statsDrawnBuckets, statsDrawnInstances,
             useFrustumCulling ? 1 : 0,
             cameraPosition.x, cameraPosition.y, cameraPosition.z);
    }
}

// ===== 草地 GPU 逐桶剔除（阶段一）=====

// 剔除 compute 管线 + 描述符布局 + 池（惰性创建一次；范式同 VulkanLightingCulling
// 的 cluster_cull：Android 上 spv 资产必须走 SDL IO 读取）。
bool TerrainRenderer::EnsureGrassCullPipeline() {
    if (m_GrassCullPipeline != VK_NULL_HANDLE) {
        return true;
    }
    if (g_Device == VK_NULL_HANDLE) {
        return false;
    }
    static const bool disabled = []{
        const char* env = std::getenv("MIKAN_GRASS_GPU_CULL");
        return env != nullptr && std::strcmp(env, "0") == 0;
    }();
    if (disabled) {
        m_GrassGpuCullDisabled = true;
        return false;
    }

    const std::string spvPath = EngineConfig::GetShaderPath("grass_cull.comp.spv");
    std::vector<char> code;
    if (SDL_IOStream* io = SDL_IOFromFile(spvPath.c_str(), "rb")) {
        const Sint64 size = SDL_GetIOSize(io);
        if (size > 0) {
            code.resize(static_cast<size_t>(size));
            if (SDL_ReadIO(io, code.data(), static_cast<size_t>(size)) != static_cast<size_t>(size)) {
                code.clear();
            }
        }
        SDL_CloseIO(io);
    }
    if (code.empty()) {
        LOGE("[TerrainRenderer] grass cull shader not found: %s", spvPath.c_str());
        return false;
    }
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        if (vkCreateShaderModule(g_Device, &info, g_Allocator, &shaderModule) != VK_SUCCESS) {
            LOGE("[TerrainRenderer] grass cull shader module creation failed");
            return false;
        }
    }

    // binding 0 = 桶 SSBO（readonly），binding 1 = 命令 SSBO（writeonly）。
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator,
                                    &m_GrassCullDescriptorLayout) != VK_SUCCESS) {
        LOGE("[TerrainRenderer] grass cull descriptor layout creation failed");
        vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(GrassCullPush);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_GrassCullDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(g_Device, &pipelineLayoutInfo, g_Allocator,
                               &m_GrassCullPipelineLayout) != VK_SUCCESS) {
        LOGE("[TerrainRenderer] grass cull pipeline layout creation failed");
        vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_GrassCullPipelineLayout;
    const VkResult pipelineResult = vkCreateComputePipelines(
        g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_GrassCullPipeline);
    vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
    if (pipelineResult != VK_SUCCESS) {
        LOGE("[TerrainRenderer] grass cull pipeline creation failed");
        return false;
    }

    // 池容量：资源数 × 帧槽位 × 重建余量；SSBO 描述符每次写入两条。
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 256;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 96;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator,
                               &m_GrassCullDescriptorPool) != VK_SUCCESS) {
        LOGE("[TerrainRenderer] grass cull descriptor pool creation failed");
        return false;
    }
    LOGI("[TerrainRenderer] grass GPU bucket culling enabled (draw via vkCmdDrawIndirect)");
    return true;
}

// 桶 SSBO / 间接命令 SSBO（host-visible，compute 写命令、CPU 写桶）。
// 容量按桶总数一次性分配，chunkCount 改动才会触发重建（waitIdle + 全槽位重建）。
bool TerrainRenderer::EnsureGrassCullBuffers(Resource& resource, uint32_t frame) {
    const uint32_t bucketTotal = static_cast<uint32_t>(resource.grassBuckets.size());
    if (bucketTotal == 0) {
        return false;
    }
    if (resource.grassGpuBucketCapacity < bucketTotal) {
        if (g_Device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_Device);
        }
        const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        bool created = true;
        for (auto& buffer : resource.grassBucketGpuBuffers) {
            buffer.Cleanup();
            created = created && buffer.Create(
                static_cast<VkDeviceSize>(bucketTotal) * sizeof(GpuGrassBucket),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory);
        }
        for (auto& buffer : resource.grassIndirectBuffers) {
            buffer.Cleanup();
            created = created && buffer.Create(
                static_cast<VkDeviceSize>(bucketTotal) * kGrassCullViewSlots *
                    sizeof(VkDrawIndirectCommand),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                memory);
        }
        if (!created) {
            LOGE("[TerrainRenderer] grass cull buffer creation failed");
            resource.grassGpuBucketCapacity = 0;
            resource.grassGpuBucketTotal = 0;
            return false;
        }
        resource.grassGpuBucketCapacity = bucketTotal;
        resource.grassGpuBucketDirty = true;
        LOGI("[TerrainRenderer] grass cull buffers sized for %u buckets", bucketTotal);
    }
    resource.grassGpuBucketTotal = bucketTotal;

    // 描述符每帧各一份（绑定对应帧槽位的桶/命令缓冲）；每次都重写，
    // 缓冲重建后无需单独失效逻辑。
    if (resource.grassCullDescriptorSets[frame] == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_GrassCullDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_GrassCullDescriptorLayout;
        if (vkAllocateDescriptorSets(g_Device, &allocInfo,
                                     &resource.grassCullDescriptorSets[frame]) != VK_SUCCESS) {
            LOGE("[TerrainRenderer] grass cull descriptor set allocation failed");
            resource.grassCullDescriptorSets[frame] = VK_NULL_HANDLE;
            return false;
        }
    }
    VkDescriptorBufferInfo bufferInfos[2]{};
    bufferInfos[0].buffer = resource.grassBucketGpuBuffers[frame].GetBuffer();
    bufferInfos[0].range = VK_WHOLE_SIZE;
    bufferInfos[1].buffer = resource.grassIndirectBuffers[frame].GetBuffer();
    bufferInfos[1].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t b = 0; b < 2; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = resource.grassCullDescriptorSets[frame];
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &bufferInfos[b];
    }
    vkUpdateDescriptorSets(g_Device, 2, writes, 0, nullptr);
    return true;
}

// 把 CPU 桶流（局部 AABB）按模型矩阵变换成世界空间，用 vkCmdUpdateBuffer 录入
// 上传命令（GPU 执行时写入，数据随命令缓冲立即快照）。不使用 host mapped memcpy：
// 本机实测 host 写入对后续提交的 GPU SSBO 读取不可见（compute 读到全 0），
// vkCmdUpdateBuffer 走设备侧写入路径，无此问题。容量 8KB ≪ 64KB 限制。
// 桶只在散布重建/chunkCount 变化/模型移动时重录，热路径零 CPU 开销。
void TerrainRenderer::UploadGrassCullBuckets(VkCommandBuffer commandBuffer,
                                             Resource& resource, uint32_t frame) {
    if (resource.grassGpuBucketTotal == 0 || commandBuffer == VK_NULL_HANDLE) {
        return;
    }
    if (!resource.grassGpuBucketDirty &&
        resource.grassGpuBucketUploadedModel == resource.model) {
        return;
    }
    std::vector<GpuGrassBucket> gpuBuckets(resource.grassGpuBucketTotal);
    for (size_t i = 0; i < resource.grassBuckets.size(); ++i) {
        const GrassChunkBucket& bucket = resource.grassBuckets[i];
        const AABB worldBounds = bucket.localBounds.Transform(resource.model);
        gpuBuckets[i].minFirst = glm::vec4(worldBounds.min, bucket.firstInstance);
        gpuBuckets[i].maxCount = glm::vec4(worldBounds.max, bucket.count);
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(gpuBuckets.size()) * sizeof(GpuGrassBucket);
    // 桶流与帧槽位无关（内容只随散布/模型变化），但必须写满全部帧槽位：
    // dirty 在一帧内清掉，若只写当前槽位，其余槽位会一直停留在上一个
    // 重建世代的副本或未初始化显存——compute 按帧槽位绑定读取，读到
    // 空/陈旧桶数据会整帧全部剔除（草周期性消失）。
    for (uint32_t s = 0; s < kFramesInFlight; ++s) {
        if (resource.grassBucketGpuBuffers[s].GetBuffer() != VK_NULL_HANDLE) {
            vkCmdUpdateBuffer(commandBuffer,
                              resource.grassBucketGpuBuffers[s].GetBuffer(), 0, bytes,
                              gpuBuckets.data());
        }
    }
    resource.grassGpuBucketDirty = false;
    resource.grassGpuBucketUploadedModel = resource.model;
}

// ===== 草地叶片级 GPU 剔除（阶段三）=====

bool TerrainRenderer::EnsureGrassBladeCullPipeline() {
    if (m_GrassBladeCullPipeline != VK_NULL_HANDLE) {
        return true;
    }
    if (g_Device == VK_NULL_HANDLE) {
        return false;
    }
    if (m_GrassGpuCullDisabled) {
        // MIKAN_GRASS_GPU_CULL=0：完全回退 CPU 逐桶，叶片级一并禁用。
        return false;
    }
    static const bool envDisabled = []{
        const char* env = std::getenv("MIKAN_GRASS_GPU_CULL");
        return env != nullptr && std::strcmp(env, "0") == 0;
    }();
    if (envDisabled) {
        m_GrassGpuCullDisabled = true;
        return false;
    }
    // 叶片级剔除默认启用（2026-09-18 验证通过：朝岛 42% / 翻转 98.4% 剔除，
    // CPU 回退链完整）。MIKAN_GRASS_COMPUTE=0 显式禁用（回退桶级/CPU 路径）。
    static const bool bladeEnvDisabled = []{
        const char* env = std::getenv("MIKAN_GRASS_COMPUTE");
        return env != nullptr && std::strcmp(env, "0") == 0;
    }();
    if (bladeEnvDisabled) {
        return false;
    }

    const std::string spvPath = EngineConfig::GetShaderPath("grass_blade_cull.comp.spv");
    std::vector<char> code;
    if (SDL_IOStream* io = SDL_IOFromFile(spvPath.c_str(), "rb")) {
        const Sint64 size = SDL_GetIOSize(io);
        if (size > 0) {
            code.resize(static_cast<size_t>(size));
            if (SDL_ReadIO(io, code.data(), static_cast<size_t>(size)) != static_cast<size_t>(size)) {
                code.clear();
            }
        }
        SDL_CloseIO(io);
    }
    if (code.empty()) {
        LOGE("[TerrainRenderer] grass blade cull shader not found: %s", spvPath.c_str());
        return false;
    }
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        if (vkCreateShaderModule(g_Device, &info, g_Allocator, &shaderModule) != VK_SUCCESS) {
            LOGE("[TerrainRenderer] grass blade cull shader module creation failed");
            return false;
        }
    }

    // binding 0 = 源实例（readonly），1 = 紧凑实例流（write），2 = 命令+原子
    // 计数（read/write），3 = 剔除参数（readonly），4 = CPU 粗筛桶列表
    // （readonly）——与 grass_blade_cull.comp 的 5-binding 布局逐字段一致；
    // push 16B（viewSlot + 源实例数 + 模式）。
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t b = 0; b < 5; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator,
                                    &m_GrassBladeCullDescriptorLayout) != VK_SUCCESS) {
        LOGE("[TerrainRenderer] grass blade cull descriptor layout creation failed");
        vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(GrassBladeCullPush);  // 16B：viewSlot + 源实例数
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_GrassBladeCullDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(g_Device, &pipelineLayoutInfo, g_Allocator,
                               &m_GrassBladeCullPipelineLayout) != VK_SUCCESS) {
        LOGE("[TerrainRenderer] grass blade cull pipeline layout creation failed");
        vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_GrassBladeCullPipelineLayout;
    const VkResult pipelineResult = vkCreateComputePipelines(
        g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_GrassBladeCullPipeline);
    vkDestroyShaderModule(g_Device, shaderModule, g_Allocator);
    if (pipelineResult != VK_SUCCESS) {
        LOGE("[TerrainRenderer] grass blade cull pipeline creation failed");
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = 128;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 64;   // 叶片级 binding5（Hi-Z）每帧槽位更新
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 48;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator,
                               &m_GrassBladeCullDescriptorPool) != VK_SUCCESS) {
        LOGE("[TerrainRenderer] grass blade cull descriptor pool creation failed");
        return false;
    }
    m_GrassBladeCullEnabled = true;
    LOGI("[TerrainRenderer] grass blade-level GPU culling enabled "
         "(compute per-blade frustum test + compacted instances + 1 indirect draw)");
    return true;
}

// 紧凑实例 / 命令+原子计数 / 参数 三类缓冲（[frame][viewSlot] 分段）。
// 紧凑流 DEVICE_LOCAL（GPU 写 GPU 读）；命令与参数 host-visible 便于调试读回。
// 容量按源实例容量分配，实例缓冲扩容才触发重建（waitIdle + 全槽位重建）。
bool TerrainRenderer::EnsureGrassBladeCullBuffers(Resource& resource, uint32_t frame,
                                                  uint32_t instanceCount) {
    if (instanceCount == 0) {
        return false;
    }
    if (resource.grassBladeCompactCapacity < instanceCount) {
        if (g_Device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_Device);
        }
        const VkMemoryPropertyFlags deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        bool created = true;
        for (auto& buffer : resource.grassBladeCompactBuffers) {
            buffer.Cleanup();
            // DEVICE_LOCAL：compute 写（原子压缩）→ 顶点阶段读，纯 GPU-GPU
            // 数据流由 barrier 保证可见性；host 无需访问。
            created = created && buffer.Create(
                static_cast<VkDeviceSize>(instanceCount) * kGrassCullViewSlots *
                    sizeof(GrassBladeInstance),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                deviceLocal);
        }
        for (auto& buffer : resource.grassBladeCmdBuffers) {
            buffer.Cleanup();
            created = created && buffer.Create(
                static_cast<VkDeviceSize>(kGrassCullViewSlots) * sizeof(VkDrawIndirectCommand),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,   // 每帧 fill 清零 + update 补写
                hostVisible);
        }
        for (auto& buffer : resource.grassBladeParamsBuffers) {
            buffer.Cleanup();
            created = created && buffer.Create(
                sizeof(GrassBladeCullParams) * kGrassCullViewSlots,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                hostVisible);
        }
        for (auto& buffer : resource.grassBladeBucketListBuffers) {
            buffer.Cleanup();
            created = created && buffer.Create(
                static_cast<VkDeviceSize>(kGrassBladeBucketListCap) * sizeof(uint32_t) * 8,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                hostVisible);
        }
        if (!created) {
            LOGE("[TerrainRenderer] grass blade cull buffer creation failed");
            resource.grassBladeCompactCapacity = 0;
            return false;
        }
        resource.grassBladeCompactCapacity = instanceCount;
        LOGI("[TerrainRenderer] grass blade cull buffers sized for %u instances", instanceCount);
    }

    if (resource.grassBladeCullDescriptorSets[frame] == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_GrassBladeCullDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_GrassBladeCullDescriptorLayout;
        if (vkAllocateDescriptorSets(g_Device, &allocInfo,
                                     &resource.grassBladeCullDescriptorSets[frame]) != VK_SUCCESS) {
            LOGE("[TerrainRenderer] grass blade cull descriptor set allocation failed");
            resource.grassBladeCullDescriptorSets[frame] = VK_NULL_HANDLE;
            return false;
        }
    }
    VkDescriptorBufferInfo bufferInfos[5]{};
    bufferInfos[0].buffer = resource.grassInstanceBuffers[frame].GetBuffer();
    bufferInfos[1].buffer = resource.grassBladeCompactBuffers[frame].GetBuffer();
    bufferInfos[2].buffer = resource.grassBladeCmdBuffers[frame].GetBuffer();
    bufferInfos[3].buffer = resource.grassBladeParamsBuffers[frame].GetBuffer();
    bufferInfos[4].buffer = resource.grassBladeBucketListBuffers[frame].GetBuffer();
    // range 必须显式写 VK_WHOLE_SIZE：零初始化 range=0 是无效描述符——无
    // validation layer 时驱动静默丢弃全部 SSBO 访问（dispatch"看似不执行"
    // 的根因，2026-09-18 定位；旧桶级管线 L2482 写了 range 故能工作）。
    for (uint32_t b = 0; b < 5; ++b) {
        bufferInfos[b].range = VK_WHOLE_SIZE;
    }
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t b = 0; b < 5; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = resource.grassBladeCullDescriptorSets[frame];
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &bufferInfos[b];
    }
    vkUpdateDescriptorSets(g_Device, 5, writes, 0, nullptr);
    return true;
}

// 2026-09-18 排查中"dispatch 被 pass 静默忽略"的理论已被证伪——真正的根因
// 是描述符 VkDescriptorBufferInfo::range 零初始化为 0，无效描述符使驱动
// 静默丢弃全部 SSBO 访问，fill/update 因 NVIDIA 的录制时拷贝语义看似执行，
// 造成"TRANSFER 生效、compute 失效"的假象）。每帧每视图各一次：reset 命令
// 段 → 写参数段 → dispatch 逐叶测试（atomicAdd 压缩进紧凑实例流 [viewSlot]
// 段）→ draw 侧 barrier。主 pass RenderGrass 按 projView 位级匹配选段，未
// dispatch 的视图自动回退 CPU 逐桶绘制。
void TerrainRenderer::RecordGrassBladeCull(VkCommandBuffer commandBuffer,
                                           const glm::mat4& view,
                                           const glm::mat4& proj, int viewSlot) {
    if (commandBuffer == VK_NULL_HANDLE || viewSlot < 0 || viewSlot >= kGrassCullViewSlots) {
        return;
    }
    if (!EnsureGrassBladeCullPipeline()) {
        return;   // 叶片级未启用（MIKAN_GRASS_COMPUTE=1 才开）→ 主 pass 回退 CPU 逐桶
    }
    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    const glm::mat4 viewProj = proj * view;
    for (Resource* resource : m_PreparedResources) {
        if (!resource || resource->grassInstanceCount == 0) {
            continue;
        }
        // 与草绘制同一惰性入口：散布重建/实例上传在此完成（含全局 Y 范围
        // grassBladeMinY/MaxY 刷新），之后 RenderGrass 再调用时为 no-op，
        // 保证命令段与实例流/参数严格同源。
        if (EnsureGrassInstancesUploaded(*resource, frame) == 0) {
            continue;
        }
        if (!EnsureGrassBladeCullBuffers(*resource, frame, resource->grassInstanceCount)) {
            continue;
        }

        auto& cullViews = resource->grassGpuCullViews[frame];
        // 本帧第一次 dispatch 前清整帧记录：上一周期未被重新 dispatch 的段
        // 自动回落 CPU 路径，避免读到陈旧命令段。
        if (resource->grassGpuCullClearedFrame != frame) {
            for (auto& cullView : cullViews) {
                cullView = Resource::GrassGpuCullView{};
            }
            resource->grassGpuCullClearedFrame = frame;
        }

        // 剔除参考系：与 RenderGrass CPU 回退 / 桶级 GPU 路径同一规则——
        // 优先 Prepare 缓存（编辑器"主相机剔除"时即主相机视锥，与地形 chunk
        // 同源），缓存无效（首帧）时用当前视图矩阵现场提平面。
        const bool useCachedCull = resource->grassUseFrustumCulling;
        const std::array<Plane, 6> cullPlanes = useCachedCull
            ? resource->grassFrustumPlanes
            : AABBUtils::ExtractFrustumPlanes(viewProj);
        const glm::vec3 camPos = useCachedCull
            ? resource->grassCameraPosition
            : glm::vec3(glm::inverse(viewProj)[3]);

        // ① 命令 SSBO 每帧 fill 清零（两视图段共用一张命令缓冲，逐视图 fill
        //   会把先 dispatch 的视图段原子计数抹零，故整帧只清一次）。
        if (resource->grassBladeCmdResetFrame != frame) {
            vkCmdFillBuffer(commandBuffer, resource->grassBladeCmdBuffers[frame].GetBuffer(),
                            0, static_cast<VkDeviceSize>(kGrassCullViewSlots) *
                                sizeof(VkDrawIndirectCommand), 0);
            resource->grassBladeCmdResetFrame = frame;
        }
        // ② 参数段：model + 6 平面 + 相机/视距 + 全局 Y 范围（局部空间）+ 叶
        //   保守半径。两视图各写各段（vkCmdUpdateBuffer 按命令序快照写入）。
        GrassBladeCullParams params{};
        params.model = resource->model;
        for (int p = 0; p < 6; ++p) {
            params.planes[p] = glm::vec4(cullPlanes[p].normal, cullPlanes[p].distance);
        }
        params.camAndDist = glm::vec4(camPos, kGrassViewDistance);
        params.heightRange = glm::vec4(resource->grassBladeMinY, resource->grassBladeMaxY,
                                       kGrassBladeCullRadius, 0.0f);
        vkCmdUpdateBuffer(commandBuffer, resource->grassBladeParamsBuffers[frame].GetBuffer(),
                          static_cast<VkDeviceSize>(viewSlot) * sizeof(GrassBladeCullParams),
                          sizeof(GrassBladeCullParams), &params);

        // ③ CPU 桶级粗筛（两级剔除的第一级）：与 CPU 逐桶绘制
        //   （RenderGrassBuckets）同一判据——桶世界 AABB 的 XZ 最近点视距 +
        //   IsInsideFrustum。幸存桶列表上传给 compute 做第二级逐叶细筛，
        //   dispatch 组数从 ceil(叶数/64)（test 场景 6286 组）降到可见桶数
        //   （典型 10-100 组），视锥外整桶的叶完全不进 GPU。桶列表溢出或
        //   缺失时回退全量 dispatch 模式（shader info.z=1，行为与粗筛引入
        //   前一致）。每桶 Y 区间随列表下发（桶内叶根部必落本桶，比全局
        //   Y 区间更紧，斜坡背面的桶细筛更容易剔）。
        struct BucketListEntry {
            uint32_t first;
            uint32_t count;
            uint32_t yLoBits;   // bit_cast<float>
            uint32_t yHiBits;
            uint32_t minXBits;  // 桶世界 XZ 包围盒（Hi-Z 整桶遮挡测试）
            uint32_t minZBits;
            uint32_t maxXBits;
            uint32_t maxZBits;
        };
        static_assert(sizeof(BucketListEntry) == 32,
                      "BucketListEntry must match grass_blade_cull.comp 2x uvec4 entry");
        // CPU 侧收集结构：比上传布局多一个排序键（桶中心到相机水平距离）。
        struct CoarseBucket {
            uint32_t first;
            uint32_t count;
            float yLo;
            float yHi;
            float dist;
            float minX;
            float minZ;
            float maxX;
            float maxZ;
        };
        static std::vector<CoarseBucket> coarseBuckets;   // 录制线程复用
        static std::vector<BucketListEntry> coarseList;   // 排序后的上传副本
        coarseBuckets.clear();
        bool coarseOverflow = false;
        uint32_t coarseSrcBlades = 0;
        if (!resource->grassBuckets.empty()) {
            const glm::vec2 camXZ(camPos.x, camPos.z);
            for (const GrassChunkBucket& bucket : resource->grassBuckets) {
                if (bucket.count == 0) {
                    continue;
                }
                const AABB worldBounds = bucket.localBounds.Transform(resource->model);
                const glm::vec2 closest = glm::clamp(
                    camXZ,
                    glm::vec2(worldBounds.min.x, worldBounds.min.z),
                    glm::vec2(worldBounds.max.x, worldBounds.max.z));
                const float dist = glm::distance(camXZ, closest);
                if (dist > kGrassViewDistance) {
                    continue;
                }
                if (!worldBounds.IsInsideFrustum(cullPlanes)) {
                    continue;
                }
                if (coarseBuckets.size() >= kGrassBladeBucketListCap) {
                    coarseOverflow = true;
                    break;
                }
                coarseBuckets.push_back({bucket.firstInstance, bucket.count,
                                         worldBounds.min.y, worldBounds.max.y, dist,
                                         worldBounds.min.x, worldBounds.min.z,
                                         worldBounds.max.x, worldBounds.max.z});
                coarseSrcBlades += bucket.count;
            }
        }
        const bool useCoarse = !coarseOverflow && !coarseBuckets.empty();
        if (useCoarse) {
            // 远→近排序：压缩流输出顺序 = 桶列表序，远草先落紧凑流先绘制，
            // 近草后画时 early-Z 直接剔掉被遮挡的远草片元（overdraw 抑制）。
            // 排序键用桶最近点距离（与距离剔除同键），几十个桶的排序零成本。
            std::sort(coarseBuckets.begin(), coarseBuckets.end(),
                      [](const CoarseBucket& a, const CoarseBucket& b) {
                          return a.dist > b.dist;
                      });
            coarseList.clear();
            coarseList.reserve(coarseBuckets.size());
            for (const CoarseBucket& cb : coarseBuckets) {
                coarseList.push_back({cb.first, cb.count,
                                      std::bit_cast<uint32_t>(cb.yLo),
                                      std::bit_cast<uint32_t>(cb.yHi),
                                      std::bit_cast<uint32_t>(cb.minX),
                                      std::bit_cast<uint32_t>(cb.minZ),
                                      std::bit_cast<uint32_t>(cb.maxX),
                                      std::bit_cast<uint32_t>(cb.maxZ)});
            }
            // 分段上传：vkCmdUpdateBuffer 单次数据上限 64KB。
            const VkDeviceSize chunkBytes =
                static_cast<VkDeviceSize>(kGrassBladeBucketUploadChunk) *
                sizeof(BucketListEntry);
            for (size_t base = 0; base < coarseList.size();
                 base += kGrassBladeBucketUploadChunk) {
                const size_t items =
                    std::min<size_t>(coarseList.size() - base, kGrassBladeBucketUploadChunk);
                vkCmdUpdateBuffer(commandBuffer,
                                  resource->grassBladeBucketListBuffers[frame].GetBuffer(),
                                  static_cast<VkDeviceSize>(base) * sizeof(BucketListEntry),
                                  static_cast<VkDeviceSize>(items) * sizeof(BucketListEntry),
                                  coarseList.data() + base);
            }
        }

        // ④ TRANSFER 写（fill/参数段/桶列表）与上一 dispatch 的 SHADER 写 →
        //   本次 compute 读写（多视图 dispatch 之间也由该 barrier 排序）。
        //   barrier 统一录在全部 TRANSFER 命令之后、dispatch 之前（spec 正确
        //   排序：barrier 只约束其之前提交的执行）。
        VkMemoryBarrier toCompute{};
        toCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &toCompute, 0, nullptr, 0, nullptr);

        // ⑤ dispatch：粗筛模式一工作组一桶（组数 = 幸存桶数，桶内 64 线程跨步）；
        //   全量回退一叶一调用。可见叶 atomicAdd 压缩进紧凑流 [viewSlot] 段。
        //   粗筛全灭（无溢出）时不 dispatch，命令段保持 fill 清零态 → 间接绘制
        //   instanceCount=0 自然空转。
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_GrassBladeCullPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_GrassBladeCullPipelineLayout, 0, 1,
                                &resource->grassBladeCullDescriptorSets[frame], 0, nullptr);
        const uint32_t cullMode = useCoarse ? 0u : 1u;
        GrassBladeCullPush push{glm::uvec4(static_cast<uint32_t>(viewSlot),
                                           resource->grassInstanceCount, cullMode, 0u)};
        vkCmdPushConstants(commandBuffer, m_GrassBladeCullPipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GrassBladeCullPush), &push);
        if (useCoarse) {
            vkCmdDispatch(commandBuffer,
                          static_cast<uint32_t>(coarseList.size()), 1, 1);
        } else if (coarseOverflow) {
            vkCmdDispatch(commandBuffer, (resource->grassInstanceCount + 63) / 64, 1, 1);
        }
        // ⑤ compute 写（紧凑流/命令段）→ 顶点属性读取 + 间接命令读取。
        VkMemoryBarrier toDraw{};
        toDraw.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toDraw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toDraw.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                               VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0, 1, &toDraw, 0, nullptr, 0, nullptr);

        cullViews[static_cast<size_t>(viewSlot)].dispatched = true;
        cullViews[static_cast<size_t>(viewSlot)].viewProj = viewProj;
        if (std::getenv("MIKAN_GRASS_CULL_STATS")) {
            LOGI("[TerrainRenderer][GrassCullStats] blade-cull frame=%u buf=%p slot=%d: "
                 "mode=%s groups=%u coarseBuckets=%zu coarseBlades=%u totalSrc=%u "
                 "cam=(%.1f,%.1f,%.1f) planes=%s",
                 frame, (void*)resource->grassBladeCmdBuffers[frame].GetBuffer(),
                 viewSlot,
                 useCoarse ? "coarse+fine" : (coarseOverflow ? "fallback-full" : "all-culled"),
                 useCoarse ? static_cast<uint32_t>(coarseList.size())
                           : (coarseOverflow ? (resource->grassInstanceCount + 63) / 64 : 0u),
                 coarseBuckets.size(), coarseSrcBlades,
                 resource->grassInstanceCount,
                 camPos.x, camPos.y, camPos.z,
                 useCachedCull ? "cached" : "live");
        }
    }
}


void TerrainRenderer::RecordGrassGpuCull(VkCommandBuffer commandBuffer,
                                         const glm::mat4& view, const glm::mat4& proj,
                                         int viewSlot) {
    if (m_GrassBladeCullEnabled) {
        // 叶片级剔除走独立入口 RecordGrassBladeCull（帧管线在 CSM 之前调用），
        // 桶级命令段机制不再参与。
        return;
    }
    if (commandBuffer == VK_NULL_HANDLE ||
        viewSlot < 0 || viewSlot >= kGrassCullViewSlots) {
        LOGD("[TerrainRenderer] RecordGrassGpuCull rejected: cb=%d slot=%d",
             commandBuffer != VK_NULL_HANDLE ? 1 : 0, viewSlot);
        return;
    }
    if (!EnsureGrassCullPipeline()) {
        LOGD("[TerrainRenderer] RecordGrassGpuCull: pipeline unavailable (disabled=%d)",
             m_GrassGpuCullDisabled ? 1 : 0);
        return;
    }
    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    glm::mat4 viewProj = proj * view;
    // 临时探针（MIKAN_GRASS_CULL_PROBE=1）：第 16 次起把剔除视锥下移 300m——
    // 全部草桶都在视锥外上方，剔除正常时统计可见桶应骤降 ≈0。验证完即删。
    static const bool s_cullProbe = []{
        const char* env = std::getenv("MIKAN_GRASS_CULL_PROBE");
        return env != nullptr && env[0] == '1';
    }();
    if (s_cullProbe) {
        static int s_cullProbeCall = 0;
        ++s_cullProbeCall;
        if (s_cullProbeCall == 16) {
            LOGI("[Probe] grass cull probe: frustum -300m Y from call %d", s_cullProbeCall);
        }
        if (s_cullProbeCall >= 16) {
            glm::mat4 probedView = view;
            probedView[3].y += 300.0f;
            viewProj = proj * probedView;
        }
    }
    for (Resource* resource : m_PreparedResources) {
        if (!resource) {
            continue;
        }
        // 与草绘制同一惰性入口：散布重建/实例上传在此完成，之后 RenderGrass
        // 再调用 EnsureGrassInstancesUploaded 时 grassDirty 已清空（no-op），
        // 保证命令段与实例流/桶流严格同源。
        if (EnsureGrassInstancesUploaded(*resource, frame) == 0) {
            continue;
        }
        if (!EnsureGrassCullBuffers(*resource, frame)) {
            continue;
        }
        UploadGrassCullBuckets(commandBuffer, *resource, frame);

        auto& cullViews = resource->grassGpuCullViews[frame];
        // 本帧第一次 dispatch 前清整帧记录：上一周期未被重新 dispatch 的段
        // （如 CSM/视图不可用）自动回落 CPU 路径，避免读到陈旧命令。
        if (resource->grassGpuCullClearedFrame != frame) {
            for (auto& cullView : cullViews) {
                cullView = Resource::GrassGpuCullView{};
            }
            resource->grassGpuCullClearedFrame = frame;
        }

        const uint32_t bucketTotal = resource->grassGpuBucketTotal;
        // 阶段一默认路径：CPU 逐桶剔除（判据与 RenderGrassBuckets 一致）+
        // vkCmdUpdateBuffer 录入命令段 + 主 pass 一条 vkCmdDrawIndirect。
        // compute 路径（MIKAN_GRASS_COMPUTE=1）：grass_cull.comp 在 GPU 上做同样的
        // 逐桶测试。此前"compute 写入不可见"的误诊根因是缺两条 barrier：
        // ① 桶上传(TRANSFER_WRITE)→compute 读(SHADER_READ)——compute 读到全 0；
        // ② compute 写(SHADER_WRITE)→间接绘制读(INDIRECT_COMMAND_READ)——绘制
        //   读到全 0 命令。两条都按规范补齐后 compute 与 CPU 路径等价。
        static const bool s_useComputeCull = []{
            const char* env = std::getenv("MIKAN_GRASS_COMPUTE");
            return env != nullptr && env[0] == '1';
        }();
        // 剔除参考系（关键）：优先复用 Prepare 缓存——与地形 chunk 的
        // chunks.UpdateVisibility 完全同源同参。编辑器场景视图开启"主相机剔除"
        // 时 Prepare 传的就是主相机视锥，草必须用同一套平面，否则场景视图里
        // 游戏视锥外的草照画而地形已剔（参考系分裂）。缓存无效（首帧/尚未有
        // 带视锥的 Prepare）或探针模式下才用钩子视图矩阵现场提平面保底。
        // 注意：主几何 Prepare 在 CSM 之后执行，缓存可能滞后一帧——主 pass
        // 位级匹配失败走回退时用的是本帧缓存，仅 GPU 段消费路径有一帧滞后。
        const bool useCachedCull = resource->grassUseFrustumCulling && !s_cullProbe;
        const std::array<Plane, 6> cullPlanes = useCachedCull
            ? resource->grassFrustumPlanes
            : AABBUtils::ExtractFrustumPlanes(viewProj);
        const glm::vec3 camPos = useCachedCull
            ? resource->grassCameraPosition
            : glm::vec3(glm::inverse(viewProj)[3]);
        if (s_useComputeCull && m_GrassCullPipeline != VK_NULL_HANDLE &&
            resource->grassCullDescriptorSets[frame] != VK_NULL_HANDLE) {
            // 读回调试（MIKAN_GRASS_COMPUTE_DEBUG=1）：读本帧槽位上一周期（3 帧前、
            // 同槽位复用 ⇒ 其提交已完成）compute 写入的命令段。注意时机：必须在
            // "提交完成后"读，录制期读还未提交的内存只会读到全 0（旧实验误诊
            // "host 写入不可见"正是读早了）。
            static const bool s_dbgReadback = []{
                const char* env = std::getenv("MIKAN_GRASS_COMPUTE_DEBUG");
                return env != nullptr && env[0] == '1';
            }();
            static int s_dbgCall = 0;
            ++s_dbgCall;
            if (s_dbgReadback && s_dbgCall >= 20 &&
                resource->grassIndirectBuffers[frame].GetBuffer() != VK_NULL_HANDLE) {
                // 调试专用：等 GPU 全部完成再读，否则读到的是正在被上一帧
                // compute 并发覆写的中间态（引擎 CPU 领先 GPU 2-3 帧）。
                vkDeviceWaitIdle(g_Device);
                if (resource->grassIndirectBuffers[frame].GetMappedPtr() == nullptr) {
                    resource->grassIndirectBuffers[frame].Map();
                }
                if (const void* mapped = resource->grassIndirectBuffers[frame].GetMappedPtr()) {
                    const VkDrawIndirectCommand* cmds =
                        static_cast<const VkDrawIndirectCommand*>(mapped);
                    uint32_t visibleCount = 0;
                    uint32_t instanceSum = 0;
                    for (uint32_t i = 0; i < bucketTotal; ++i) {
                        if (cmds[i].instanceCount > 0) {
                            ++visibleCount;
                            instanceSum += cmds[i].instanceCount;
                        }
                    }
                    LOGI("[TerrainRenderer][GrassComputeDbg] readback call=%d slot=%d: "
                         "visible=%u instanceSum=%u first={vc=%u ic=%u fi=%u}",
                         s_dbgCall, viewSlot, visibleCount, instanceSum,
                         cmds[0].vertexCount, cmds[0].instanceCount,
                         cmds[0].firstInstance);
                    {
                        // 差分对比：slot-0 段 = 上一周期 compute 输出，slot-1 段 =
                        // 上一周期 CPU 期望值（debug 模式每帧写入，headless 不用
                        // slot-1）。静态相机下两者应逐命令一致。
                        const VkDrawIndirectCommand* gpuSeg = cmds;
                        const VkDrawIndirectCommand* cpuSeg = cmds + bucketTotal;
                        uint32_t mismatch = 0;
                        int logged = 0;
                        for (uint32_t i = 0; i < bucketTotal; ++i) {
                            if (gpuSeg[i].instanceCount != cpuSeg[i].instanceCount ||
                                gpuSeg[i].firstInstance != cpuSeg[i].firstInstance) {
                                ++mismatch;
                                if (logged < 3) {
                                    ++logged;
                                    LOGI("[TerrainRenderer][GrassComputeDbg]   mismatch[%u]: "
                                         "gpu={ic=%u fi=%u} cpu={ic=%u fi=%u}",
                                         i, gpuSeg[i].instanceCount, gpuSeg[i].firstInstance,
                                         cpuSeg[i].instanceCount, cpuSeg[i].firstInstance);
                                }
                            }
                        }
                        LOGI("[TerrainRenderer][GrassComputeDbg]   compare: mismatch=%u/%u",
                             mismatch, bucketTotal);
                    }
                    {
                        const uint32_t* raw = reinterpret_cast<const uint32_t*>(cmds);
                        LOGI("[TerrainRenderer][GrassComputeDbg]   sizeof(cmd)=%zu "
                             "seg0 raw[0..23]: "
                             "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u "
                             "%u %u %u %u %u %u %u %u",
                             sizeof(VkDrawIndirectCommand),
                             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                             raw[6], raw[7], raw[8], raw[9], raw[10], raw[11],
                             raw[12], raw[13], raw[14], raw[15], raw[16], raw[17],
                             raw[18], raw[19], raw[20], raw[21], raw[22], raw[23]);
                        // 跨段边界（byte 3072 = uint 768）前后各 8 个 uint
                        LOGI("[TerrainRenderer][GrassComputeDbg]   boundary raw[764..787]: "
                             "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u "
                             "%u %u %u %u %u %u %u %u",
                             raw[764], raw[765], raw[766], raw[767], raw[768],
                             raw[769], raw[770], raw[771], raw[772], raw[773],
                             raw[774], raw[775], raw[776], raw[777], raw[778],
                             raw[779], raw[780], raw[781], raw[782], raw[783],
                             raw[784], raw[785], raw[786], raw[787]);
                    }
                    if (resource->grassBucketGpuBuffers[frame].GetMappedPtr() == nullptr) {
                        resource->grassBucketGpuBuffers[frame].Map();
                    }
                    if (const GpuGrassBucket* bk =
                            static_cast<const GpuGrassBucket*>(
                                resource->grassBucketGpuBuffers[frame].GetMappedPtr())) {
                        for (uint32_t bi = 0; bi < 4; ++bi) {
                            LOGI("[TerrainRenderer][GrassComputeDbg]   bucket[%u]: "
                                 "min=(%.1f,%.1f,%.1f) fi=%.0f max=(%.1f,%.1f,%.1f) cnt=%.0f",
                                 bi, bk[bi].minFirst.x, bk[bi].minFirst.y, bk[bi].minFirst.z,
                                 bk[bi].minFirst.w, bk[bi].maxCount.x, bk[bi].maxCount.y,
                                 bk[bi].maxCount.z, bk[bi].maxCount.w);
                        }
                    }
                }
            }
            // ① 桶 SSBO 上传（本命令缓冲更早处 vkCmdUpdateBuffer）→ compute 读取
            VkMemoryBarrier uploadBarrier{};
            uploadBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            uploadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            uploadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &uploadBarrier, 0, nullptr, 0, nullptr);
            GrassCullPush push{};
            for (int p = 0; p < 6; ++p) {
                push.planes[p] = glm::vec4(cullPlanes[p].normal, cullPlanes[p].distance);
            }
            push.cameraPosDist = glm::vec4(camPos, kGrassViewDistance);
            push.params = glm::uvec4(bucketTotal, static_cast<uint32_t>(viewSlot), 0u, 0u);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              m_GrassCullPipeline);
            VkDescriptorSet cullSet = resource->grassCullDescriptorSets[frame];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_GrassCullPipelineLayout, 0, 1, &cullSet, 0, nullptr);
            vkCmdPushConstants(commandBuffer, m_GrassCullPipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(GrassCullPush), &push);
            vkCmdDispatch(commandBuffer, (bucketTotal + 63) / 64, 1, 1);
            // 差分基准（debug 专用）：CPU 期望命令写进 slot-1 段，供下一周期读回对比。
            if (s_dbgReadback) {
                std::vector<VkDrawIndirectCommand> expectedCmds(bucketTotal);
                for (uint32_t i = 0; i < bucketTotal; ++i) {
                    const GrassChunkBucket& bucket = resource->grassBuckets[i];
                    const AABB worldBounds = bucket.localBounds.Transform(resource->model);
                    bool visible = worldBounds.IsInsideFrustum(cullPlanes);
                    if (visible) {
                        const glm::vec2 camXZ(camPos.x, camPos.z);
                        const glm::vec2 closest = glm::clamp(camXZ,
                            glm::vec2(worldBounds.min.x, worldBounds.min.z),
                            glm::vec2(worldBounds.max.x, worldBounds.max.z));
                        if (glm::distance(camXZ, closest) > kGrassViewDistance) {
                            visible = false;
                        }
                    }
                    expectedCmds[i].vertexCount = 10;
                    expectedCmds[i].instanceCount = visible ? bucket.count : 0;
                    expectedCmds[i].firstInstance = bucket.firstInstance;
                }
                vkCmdUpdateBuffer(commandBuffer,
                                  resource->grassIndirectBuffers[frame].GetBuffer(),
                                  static_cast<VkDeviceSize>(bucketTotal) *
                                      sizeof(VkDrawIndirectCommand),
                                  static_cast<VkDeviceSize>(bucketTotal) *
                                      sizeof(VkDrawIndirectCommand),
                                  expectedCmds.data());
            }
            // ② compute 写命令段 → 主 pass vkCmdDrawIndirect 读取（跨命令缓冲、
            // 同队列先后提交，execution ordering 依然成立）
            VkMemoryBarrier cullBarrier{};
            cullBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            cullBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            cullBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                 0, 1, &cullBarrier, 0, nullptr, 0, nullptr);
            if (std::getenv("MIKAN_GRASS_CULL_STATS")) {
                LOGI("[TerrainRenderer][GrassCullStats] gpu-cull slot=%d: compute dispatch groups=%u buckets=%u cam=(%.1f,%.1f,%.1f)",
                     viewSlot, (bucketTotal + 63) / 64, bucketTotal,
                     camPos.x, camPos.y, camPos.z);
            }
        } else {
            std::vector<VkDrawIndirectCommand> cpuCmds(bucketTotal);
            uint32_t statsVisibleBuckets = 0;
            uint32_t statsVisibleInstances = 0;
            for (uint32_t i = 0; i < bucketTotal; ++i) {
                const GrassChunkBucket& bucket = resource->grassBuckets[i];
                const AABB worldBounds = bucket.localBounds.Transform(resource->model);
                bool visible = worldBounds.IsInsideFrustum(cullPlanes);
                if (visible) {
                    const glm::vec2 camXZ(camPos.x, camPos.z);
                    const glm::vec2 closest = glm::clamp(camXZ,
                        glm::vec2(worldBounds.min.x, worldBounds.min.z),
                        glm::vec2(worldBounds.max.x, worldBounds.max.z));
                    if (glm::distance(camXZ, closest) > kGrassViewDistance) {
                        visible = false;
                    }
                }
                if (visible) {
                    ++statsVisibleBuckets;
                    statsVisibleInstances += bucket.count;
                }
                cpuCmds[i].vertexCount = 10;
                cpuCmds[i].instanceCount = visible ? bucket.count : 0;
                cpuCmds[i].firstInstance = bucket.firstInstance;
            }
            if (std::getenv("MIKAN_GRASS_CULL_STATS")) {
                LOGI("[TerrainRenderer][GrassCullStats] gpu-cull slot=%d: buckets=%u visible=%u instances=%u cam=(%.1f,%.1f,%.1f)",
                     viewSlot, bucketTotal, statsVisibleBuckets, statsVisibleInstances,
                     camPos.x, camPos.y, camPos.z);
            }
            vkCmdUpdateBuffer(commandBuffer,
                              resource->grassIndirectBuffers[frame].GetBuffer(),
                              static_cast<VkDeviceSize>(viewSlot) *
                                  static_cast<VkDeviceSize>(bucketTotal) *
                                  sizeof(VkDrawIndirectCommand),
                              static_cast<VkDeviceSize>(bucketTotal) *
                                  sizeof(VkDrawIndirectCommand),
                              cpuCmds.data());
            // CPU 写命令段 → 间接绘制读取：host 或 update-buffer 写入后需要
            // execution/memory barrier 保证 draw 侧读到完整数据。
            VkMemoryBarrier cmdBarrier{};
            cmdBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            cmdBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            cmdBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                 0, 1, &cmdBarrier, 0, nullptr, 0, nullptr);
        }

        cullViews[static_cast<size_t>(viewSlot)].dispatched = true;
        cullViews[static_cast<size_t>(viewSlot)].viewProj = viewProj;
    }
}

void TerrainRenderer::CleanupGrassCullResources() {
    if (g_Device == VK_NULL_HANDLE) {
        return;
    }
    if (m_GrassCullPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_Device, m_GrassCullPipeline, g_Allocator);
        m_GrassCullPipeline = VK_NULL_HANDLE;
    }
    if (m_GrassCullPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_Device, m_GrassCullPipelineLayout, g_Allocator);
        m_GrassCullPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_GrassCullDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_GrassCullDescriptorPool, g_Allocator);
        m_GrassCullDescriptorPool = VK_NULL_HANDLE;
    }
    if (m_GrassCullDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_GrassCullDescriptorLayout, g_Allocator);
        m_GrassCullDescriptorLayout = VK_NULL_HANDLE;
    }
    // 叶片级剔除管线对象（缓冲在 Resource 里随资源销毁）。
    if (m_GrassBladeCullPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_Device, m_GrassBladeCullPipeline, g_Allocator);
        m_GrassBladeCullPipeline = VK_NULL_HANDLE;
    }
    if (m_GrassBladeCullPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_Device, m_GrassBladeCullPipelineLayout, g_Allocator);
        m_GrassBladeCullPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_GrassBladeCullDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_GrassBladeCullDescriptorPool, g_Allocator);
        m_GrassBladeCullDescriptorPool = VK_NULL_HANDLE;
    }
    if (m_GrassBladeCullDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_GrassBladeCullDescriptorLayout, g_Allocator);
        m_GrassBladeCullDescriptorLayout = VK_NULL_HANDLE;
    }
    m_GrassBladeCullEnabled = false;
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
            RenderGrass(commandBuffer, *resource, frame, projView);

            // 水面网格画在草之后：不透明、写深度，地形/草已写深度后，
            // 干燥区域（水面顶点下沉到地形之下）被深度测试整块剔除。
            if (m_WaterPipeline.GetPipeline() != VK_NULL_HANDLE &&
                resource->waterPatch.indexCount > 0) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_WaterPipeline.GetPipeline());
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_WaterPipeline.GetLayout(), 0, 1,
                                        &resource->descriptorSets[frame], 0, nullptr);
                VkDeviceSize waterOffset = 0;
                VkBuffer waterVertexBuffer = resource->waterPatch.vertexBuffer.GetBuffer();
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &waterVertexBuffer, &waterOffset);
                vkCmdBindIndexBuffer(commandBuffer,
                                     resource->waterPatch.indexBuffer.GetBuffer(),
                                     0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, resource->waterPatch.indexCount, 1, 0, 0, 0);
            }

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
    resource.heightmapPaintedDirty = true;

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
    resource.controlPaintedDirty = true;

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
    resource.grassPaintedDirty = true;

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

bool TerrainRenderer::GetWaterMapInfo(ECS::Entity entity, uint32_t& outWidth,
                                      uint32_t& outHeight, bool& outPaintable) const {
    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    const Resource& resource = *it->second;
    outWidth = resource.waterWidth;
    outHeight = resource.waterHeight;
    outPaintable = !resource.waterCpu.empty() && !resource.waterKey.empty();
    return true;
}

bool TerrainRenderer::PaintTerrainWaterWorld(ECS::Entity entity, float worldX, float worldZ,
                                             float radius, float targetDepth,
                                             float hardness, float amount) {
    if (radius <= 0.0f || amount <= 0.0f) {
        return false;
    }

    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }
    Resource& resource = *it->second;
    if (resource.waterCpu.empty() || resource.waterWidth < 2 || resource.waterHeight < 2 ||
        resource.waterKey.empty()) {
        return false;
    }
    resource.waterPaintedDirty = true;

    const glm::vec2 worldSize = resource.settings.worldSize;
    const glm::vec3 local = WorldToTerrainLocal(resource.model, glm::vec3(worldX, 0.0f, worldZ));

    const float maxTexelX = static_cast<float>(resource.waterWidth - 1);
    const float maxTexelY = static_cast<float>(resource.waterHeight - 1);
    const float texelsPerWorldX = maxTexelX / std::max(worldSize.x, 1e-4f);
    const float texelsPerWorldZ = maxTexelY / std::max(worldSize.y, 1e-4f);

    // 与材质/草地笔刷同一套顶左原点行序映射。
    const float centerTexelX = std::clamp(local.x / worldSize.x + 0.5f, 0.0f, 1.0f) * maxTexelX;
    const float centerTexelY = (1.0f - std::clamp(local.z / worldSize.y + 0.5f, 0.0f, 1.0f)) * maxTexelY;
    const float radiusTexelX = radius * texelsPerWorldX;
    const float radiusTexelY = radius * texelsPerWorldZ;

    const int minX = std::max(0, static_cast<int>(std::floor(centerTexelX - radiusTexelX)));
    const int maxX = std::min(static_cast<int>(resource.waterWidth) - 1,
                              static_cast<int>(std::ceil(centerTexelX + radiusTexelX)));
    const int minY = std::max(0, static_cast<int>(std::floor(centerTexelY - radiusTexelY)));
    const int maxY = std::min(static_cast<int>(resource.waterHeight) - 1,
                              static_cast<int>(std::ceil(centerTexelY + radiusTexelY)));
    if (minX > maxX || minY > maxY) {
        return false;
    }

    // 硬度语义与材质/草地笔刷完全一致：平顶核心 + smoothstep 过渡带。
    const float texelWorld = std::min(std::abs(worldSize.x) / maxTexelX,
                                      std::abs(worldSize.y) / maxTexelY);
    const float band = std::max(radius * (1.0f - std::clamp(hardness, 0.0f, 1.0f)),
                                std::max(texelWorld * 1.5f, 1e-4f));
    const float coreRadius = std::max(radius - band, 0.0f);

    const float inverseBand = 1.0f / band;
    const float paintedAmount = std::clamp(amount, 0.0f, 1.0f);
    const float targetValue = std::clamp(targetDepth, 0.0f, 1.0f) * 255.0f;
    bool changed = false;

    // 涂水同步挖湖盆：本帧新增多少米水深，地形就降低多少米，这样水面
    // 正好贴在"涂水前的原地面"高度上（湖盆凹陷感来自地形几何本身）。
    // 水深只增不减——擦除水位不会把湖底填回来（无法恢复原始地表）。
    // 水位图与高度图同分辨率同原点，同一矩形直接复用。
    const bool canDig = resource.heightmapCpu.size() ==
                            static_cast<size_t>(resource.waterWidth) * resource.waterHeight &&
                        resource.settings.heightScale != 0.0f;
    const float samplesPerMeter = canDig ? 65535.0f / resource.settings.heightScale : 0.0f;
    bool heightChanged = false;

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

            uint8_t& texel = resource.waterCpu[static_cast<size_t>(y) * resource.waterWidth +
                                                static_cast<size_t>(x)];
            const float oldWater = static_cast<float>(texel);
            const float updated = oldWater + (targetValue - oldWater) * blend;
            const uint8_t clamped = static_cast<uint8_t>(std::lround(std::clamp(updated, 0.0f, 255.0f)));
            if (clamped != texel) {
                texel = clamped;
                changed = true;

                if (canDig && clamped > oldWater) {
                    // 水深增加 → 同步把地形挖低同等米数（高度图归一化样本）。
                    const float depthMeters =
                        (static_cast<float>(clamped) - oldWater) / 255.0f * kTerrainWaterMaxDepth;
                    const size_t hIndex = static_cast<size_t>(y) * resource.heightmapWidth +
                                          static_cast<size_t>(x);
                    const float lowered =
                        static_cast<float>(resource.heightmapCpu[hIndex]) - depthMeters * samplesPerMeter;
                    const uint16_t hClamped =
                        static_cast<uint16_t>(std::clamp(lowered, 0.0f, 65535.0f));
                    if (hClamped != resource.heightmapCpu[hIndex]) {
                        resource.heightmapCpu[hIndex] = hClamped;
                        heightChanged = true;
                    }
                }
            }
        }
    }

    if (!changed) {
        return false;
    }

    // 湖盆挖低部分回写高度图（与水位图同一矩形；失败必须响亮报错）。
    if (heightChanged) {
        resource.heightmapPaintedDirty = true;
        if (g_TexturePool) {
            const bool heightUploaded = g_TexturePool->UpdateHeightmapRegion16(
                resource.heightmapKey,
                static_cast<uint32_t>(minX), static_cast<uint32_t>(minY),
                static_cast<uint32_t>(maxX - minX + 1), static_cast<uint32_t>(maxY - minY + 1),
                resource.heightmapCpu.data(), resource.heightmapWidth);
            if (!heightUploaded) {
                LOGE("[TerrainRenderer] water brush: heightmap dig region upload failed "
                     "(entity=%u key=%s rect=%d,%d %dx%d)",
                     static_cast<unsigned>(entity), resource.heightmapKey.c_str(),
                     minX, minY, maxX - minX + 1, maxY - minY + 1);
                return false;
            }
        }
    }

    if (g_TexturePool) {
        const bool uploaded = g_TexturePool->UpdateGrassMaskRegion8(
            resource.waterKey,
            static_cast<uint32_t>(minX), static_cast<uint32_t>(minY),
            static_cast<uint32_t>(maxX - minX + 1), static_cast<uint32_t>(maxY - minY + 1),
            resource.waterCpu.data(), resource.waterWidth);
        if (!uploaded) {
            LOGE("[TerrainRenderer] water brush: water mask region upload failed "
                 "(entity=%u key=%s rect=%d,%d %dx%d)",
                 static_cast<unsigned>(entity), resource.waterKey.c_str(),
                 minX, minY, maxX - minX + 1, maxY - minY + 1);
            return false;
        }
    }
    // 水下不长草：水位变了就触发草实例流重建（下一帧 Prepare 按水位图
    // 剔除水下 texel），涂水区域内已有的草叶下一帧消失。
    resource.grassDirty = true;
    return true;
}

// ===== 显式保存：笔刷产物导出 =====
// 数据源是 CPU 镜像（与屏幕渲染一致），写出 16-bit/8-bit PNG 并清对应脏标记。
// 返回约定：1 = 已写出，0 = 没有改动（非错误），-1 = 失败。

int TerrainRenderer::ExportSculptedHeightmap(ECS::Entity entity, const std::string& absolutePath,
                                             std::string* errorMessage) {
    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return 0;
    }
    Resource& resource = *it->second;
    if (!resource.heightmapPaintedDirty || resource.heightmapCpu.empty() ||
        resource.heightmapWidth < 2 || resource.heightmapHeight < 2) {
        return 0;
    }
    if (!HeightmapLoader::SavePng16(absolutePath, resource.heightmapWidth,
                                    resource.heightmapHeight,
                                    resource.heightmapCpu.data(), errorMessage)) {
        LOGE("[TerrainRenderer] failed to save sculpted heightmap for entity %u: %s",
                    static_cast<unsigned>(entity), absolutePath.c_str());
        return -1;
    }
    resource.heightmapPaintedDirty = false;
    LOGI("[TerrainRenderer] saved sculpted heightmap for entity %u: %s",
                static_cast<unsigned>(entity), absolutePath.c_str());
    return 1;
}

int TerrainRenderer::ExportPaintedControlMap(ECS::Entity entity, const std::string& absolutePath,
                                             std::string* errorMessage) {
    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return 0;
    }
    Resource& resource = *it->second;
    if (!resource.controlPaintedDirty || resource.controlCpu.empty() ||
        resource.controlWidth < 2 || resource.controlHeight < 2) {
        return 0;
    }
    if (!HeightmapLoader::SavePng8(absolutePath, resource.controlWidth,
                                   resource.controlHeight, 4,
                                   resource.controlCpu.data(), errorMessage)) {
        LOGE("[TerrainRenderer] failed to save painted control map for entity %u: %s",
                    static_cast<unsigned>(entity), absolutePath.c_str());
        return -1;
    }
    resource.controlPaintedDirty = false;
    LOGI("[TerrainRenderer] saved painted control map for entity %u: %s",
                static_cast<unsigned>(entity), absolutePath.c_str());
    return 1;
}

int TerrainRenderer::ExportPaintedGrassMap(ECS::Entity entity, const std::string& absolutePath,
                                           std::string* errorMessage) {
    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return 0;
    }
    Resource& resource = *it->second;
    if (!resource.grassPaintedDirty || resource.grassCpu.empty() ||
        resource.grassWidth < 2 || resource.grassHeight < 2) {
        return 0;
    }
    if (!HeightmapLoader::SavePng8(absolutePath, resource.grassWidth,
                                   resource.grassHeight, 1,
                                   resource.grassCpu.data(), errorMessage)) {
        LOGE("[TerrainRenderer] failed to save painted grass map for entity %u: %s",
                    static_cast<unsigned>(entity), absolutePath.c_str());
        return -1;
    }
    resource.grassPaintedDirty = false;
    LOGI("[TerrainRenderer] saved painted grass map for entity %u: %s",
                static_cast<unsigned>(entity), absolutePath.c_str());
    return 1;
}

int TerrainRenderer::ExportPaintedWaterMap(ECS::Entity entity, const std::string& absolutePath,
                                           std::string* errorMessage) {
    auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return 0;
    }
    Resource& resource = *it->second;
    if (!resource.waterPaintedDirty || resource.waterCpu.empty() ||
        resource.waterWidth < 2 || resource.waterHeight < 2) {
        return 0;
    }
    if (!HeightmapLoader::SavePng8(absolutePath, resource.waterWidth,
                                   resource.waterHeight, 1,
                                   resource.waterCpu.data(), errorMessage)) {
        LOGE("[TerrainRenderer] failed to save painted water map for entity %u: %s",
                    static_cast<unsigned>(entity), absolutePath.c_str());
        return -1;
    }
    resource.waterPaintedDirty = false;
    LOGI("[TerrainRenderer] saved painted water map for entity %u: %s",
                static_cast<unsigned>(entity), absolutePath.c_str());
    return 1;
}
