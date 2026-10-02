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
#include "Rendering/HiZHistory.h"
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
std::array<Plane, 6> UnrestrictedGrassPlanes() {
    std::array<Plane, 6> planes{};
    for (auto& plane : planes) {
        plane.normal = glm::vec3(0.0f);
        plane.distance = 0.0f;
    }
    return planes;
}
uint64_t GrassHiZOccluderRevision(const RenderWorld& world) {
    uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
    mix(world.entitySetVersion);
    for (const auto& entity : world.entities) {
        if (!entity.hasMesh && !entity.hasTerrain) continue;
        mix(entity.entity);
        mix(entity.visible);
        mix(entity.hasTransform);
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                mix(std::bit_cast<uint32_t>(entity.transform.worldMatrix[c][r]));
            }
        }
        // Capture fingerprints include direct field writes without dirty events.
        for (const auto component : {RenderWorldCaptureComponent::Mesh,
                RenderWorldCaptureComponent::Material,
                RenderWorldCaptureComponent::RenderFlags,
                RenderWorldCaptureComponent::Terrain}) {
            const auto index = static_cast<size_t>(component);
            mix(entity.capturedComponentPresence[index]);
            mix(entity.capturedComponentRevisions[index]);
        }
    }
    return hash;
}
} // namespace

// Grass generation, rendering, and GPU culling.
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
    resource.grassCullCacheValid = false;
    resource.grassVisibleBucketMask.clear();
    resource.grassCullCandidates.clear();
    resource.grassCullCoarseOverflow = false;
    resource.grassCullCandidateBlades = 0;
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
            const uint32_t maxSampleY = hh - 1u;
            const uint32_t localSampleY0 = static_cast<uint32_t>(
                (static_cast<uint64_t>(cz) * maxSampleY) /
                static_cast<uint32_t>(bucketCount));
            const uint32_t localSampleY1 = static_cast<uint32_t>(
                (static_cast<uint64_t>(cz + 1) * maxSampleY) /
                static_cast<uint32_t>(bucketCount));
            // CPU mirror is stored in top-left image-row order, while the
            // uploaded height texture is bottom-up and grass local z/v=0
            // samples the bottom image row. Reverse the interval so each
            // bucket AABB follows the grass roots rendered by the shader.
            const uint32_t sourceSampleY0 = maxSampleY - localSampleY1;
            const uint32_t sourceSampleY1 = maxSampleY - localSampleY0;
            const uint32_t z0 = sourceSampleY0 > 0 ? sourceSampleY0 - 1u : 0u;
            const uint32_t z1 = std::min(maxSampleY, sourceSampleY1 + 1u);
            uint16_t minV = 0xffffu;
            uint16_t maxV = 0;
            for (uint32_t z = z0; z <= z1; ++z) {
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
                                  uint32_t frame, const glm::mat4& projView,
                                  int viewSlot, int probeFace) {
    // 反射探针（第三视图）不画草。三条理由，按重要性排序：
    //
    // 1. 成本 6 倍放大且全是 CPU 侧。探针从不为 slot 2 录制草剔除 dispatch
    //    （见 RenderSceneProbeCapture 的说明），因此每面都落进最下面的 CPU
    //    逐桶回退：遍历全部草桶做视锥+距离剔除，再**逐桶发一条 draw**。6 个面
    //    每帧重复一遍 ⇒ 帧率主要损失项。
    // 2. 收益近似为零。探针面只有 256²，草在这种尺度上的倒影是几个像素级的
    //    扰动，水面反射里根本看不出。
    // 3. 语义上也站得住：草是薄片几何，对低频反射（IBL / 场景倒影）的贡献
    //    本就该由地形+模型的低频项承担。
    //
    // 早退必须是**整个 RenderGrass 的最前面**（早于 EnsureGrassInstancesUploaded）：
    // 那个函数在 grassDirty 时会重建散布并往全部帧槽位写实例流，对不画草的
    // 视图调用只是白跑 CPU。
    if (IsProbeViewSlot(viewSlot)) {
        if (TerrainDiagEnabled()) {
            LOGI("[TerrainDiag] RenderGrass skipped: slot=%d face=%d (probe view)",
                 viewSlot, probeFace);
        }
        return;
    }
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
    // 草顶点着色器从 UBO 取 projView，因此必须绑本视图那一份：探针面若绑主集，
    // 草会被主相机矩阵画到视锥外（表现为探针里草地整体消失）。
    const VkDescriptorSet viewSet = ViewDescriptorSet(resource, frame, viewSlot, probeFace);
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
                            &viewSet, 0, nullptr);
    VkBuffer instanceBuffer = resource.grassInstanceBuffers[frame].GetBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &instanceBuffer, &offset);

    // ===== 剔除参考系解析（叶片级 / GPU 桶级 / CPU 回退三分支共用）=====
    // 优先复用 Prepare 缓存（与地形 chunk 严格同源——编辑器场景视图开启"主相机
    // 剔除"时，Prepare 传的就是主相机视锥，草必须用同一套平面，否则场景视图里
    // 游戏视锥外的草照画而地形已剔）。缓存无效（首帧/尚未有带视锥的 Prepare）
    // 时用当前视图 projView 现场提取平面兜底，相机位置从 projView 逆矩阵提取，
    // 与剔除平面严格同源。
    //
    // 两个「probe」在这里必须分清，命名已刻意区分以免再混：
    //   * cullDebugProbe / cullDebugProbeActive —— env 开关 MIKAN_GRASS_CULL_PROBE
    //     的调试偏移，把视锥人为压低 300m 验证剔除数学。
    //   * 反射探针（第三个视图）—— 由 IsProbeViewSlot(viewSlot) 判定。它**不读**
    //     这份缓存：缓存里只有主视图视锥，探针面复用它会让背向主相机的那些面
    //     把草整片剔掉。
    glm::mat4 renderProjView = projView;
    static const bool cullDebugProbe = []{
        const char* env = std::getenv("MIKAN_GRASS_CULL_PROBE");
        return env != nullptr && env[0] == '1';
    }();
    bool cullDebugProbeActive = false;
    if (cullDebugProbe) {
        static int s_fallbackProbeCall = 0;
        ++s_fallbackProbeCall;
        if (s_fallbackProbeCall == 16) {
            LOGI("[Probe] grass cull probe: frustum -300m Y from call %d",
                 s_fallbackProbeCall);
        }
        if (s_fallbackProbeCall >= 16) {
            renderProjView[3].y += 300.0f;
            cullDebugProbeActive = true; // 强制走现场平面分支，保持剔除数学可验证
        }
    }
    const glm::vec3 fallbackCam(glm::inverse(renderProjView)[3]);
    const std::array<Plane, 6> fallbackPlanes = g_SceneRenderer.IsGrassFrustumCullingEnabled()
        ? AABBUtils::ExtractFrustumPlanes(renderProjView) : UnrestrictedGrassPlanes();
    const bool useCachedCull =
        resource.grassUseFrustumCulling && !cullDebugProbeActive && !IsProbeViewSlot(viewSlot) &&
        g_SceneRenderer.IsGrassFrustumCullingEnabled();
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
            if (!cullView.dispatched || cullView.viewProj != projView ||
                cullView.renderEpoch != g_SceneRenderer.GetRenderWorld().frameNumber) {
                continue;
            }
            // 紧凑流段偏移与命令段偏移必须与 compute 写入侧一致
            // A second raster view may reference the first view's result segment.
            const int resultSlot = cullView.bladeResultSlot >= 0 ? cullView.bladeResultSlot : slot;
            VkBuffer compactBuf = resource.grassBladeCompactBuffers[frame].GetBuffer();
            VkDeviceSize compactOff = static_cast<VkDeviceSize>(resultSlot) *
                                      static_cast<VkDeviceSize>(resource.grassBladeCompactCapacity) *
                                      sizeof(GrassBladeInstance);
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &compactBuf, &compactOff);
            vkCmdDrawIndirect(commandBuffer, resource.grassBladeCmdBuffers[frame].GetBuffer(),
                              static_cast<VkDeviceSize>(resultSlot) * sizeof(VkDrawIndirectCommand),
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
                         (void*)resource.grassBladeCmdBuffers[frame].GetBuffer(), resultSlot,
                         raw[resultSlot * 4 + 0], raw[resultSlot * 4 + 1],
                         raw[resultSlot * 4 + 2], raw[resultSlot * 4 + 3]);
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
            if (view.dispatched && view.viewProj == projView &&
                view.renderEpoch == g_SceneRenderer.GetRenderWorld().frameNumber) {
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
                                &resource->shadowDescriptorSets[frame], 0, nullptr);
        VkBuffer instanceBuffer = resource->grassInstanceBuffers[frame].GetBuffer();
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &instanceBuffer, &offset);
        // 草的阴影投射按当前级联的光视锥逐桶剔除：从光空间 projView 现场提取
        // 6 平面（ortho 与透视矩阵通用），与地形 CSM 的实例级准备相互独立。
        // 再叠加主相机视锥门：级联光视锥比主相机视锥宽，相机背后/视野外的
        // 草不再收进草影（用户拍板：仅视锥体内收集）。
        const std::array<Plane, 6> lightPlanes = AABBUtils::ExtractFrustumPlanes(shadowProjView);
        RenderGrassBuckets(commandBuffer, *resource, lightPlanes, true, cameraPosition, "csm",
                           g_SceneRenderer.IsGrassFrustumCullingEnabled() ? &mainCameraFrustum : nullptr);
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
    const bool canUseCachedResult = useFrustumCulling && extraFrustum == nullptr;
    if (canUseCachedResult) {
        EnsureGrassCullCache(resource, cameraPosition, frustumPlanes);
    }

    for (size_t bucketIndex = 0; bucketIndex < resource.grassBuckets.size(); ++bucketIndex) {
        const GrassChunkBucket& bucket = resource.grassBuckets[bucketIndex];
        if (bucket.count == 0) {
            continue;
        }
        if (canUseCachedResult) {
            if (bucketIndex >= resource.grassVisibleBucketMask.size() ||
                resource.grassVisibleBucketMask[bucketIndex] == 0u) {
                continue;
            }
        } else if (useFrustumCulling) {
            const AABB worldBounds = bucket.localBounds.Transform(resource.model);
            // 距离剔除：桶 AABB 最近点到相机的水平距离超出视距即整桶不发——
            // 桶内叶片在顶点着色器里也会被视距剔掉，这里省掉整桶的 VS 调用。
            const glm::vec2 camXZ(cameraPosition.x, cameraPosition.z);
            const glm::vec2 closest = glm::clamp(
                camXZ,
                glm::vec2(worldBounds.min.x, worldBounds.min.z),
                glm::vec2(worldBounds.max.x, worldBounds.max.z));
            if (glm::distance(camXZ, closest) > kGrassViewDistance ||
                !worldBounds.IsInsideFrustum(frustumPlanes)) {
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

bool TerrainRenderer::EnsureGrassCullCache(
    Resource& resource, const glm::vec3& cameraPosition,
    const std::array<Plane, 6>& frustumPlanes) {
    const size_t bucketTotal = resource.grassBuckets.size();
    const bool cacheHit = resource.grassCullCacheValid &&
                          resource.grassVisibleBucketMask.size() == bucketTotal &&
                          SameVec3(resource.grassCullCachedCameraPosition, cameraPosition) &&
                          SameMat4(resource.grassCullCachedModel, resource.model) &&
                          SameFrustum(resource.grassCullCachedFrustumPlanes,
                                      frustumPlanes);
    if (cacheHit) {
        return true;
    }

    resource.grassCullCacheValid = false;
    resource.grassVisibleBucketMask.assign(bucketTotal, 0u);
    resource.grassCullCandidates.clear();
    resource.grassCullCandidates.reserve(
        std::min(bucketTotal, static_cast<size_t>(kGrassBladeBucketListCap)));
    resource.grassCullCoarseOverflow = false;
    resource.grassCullCandidateBlades = 0;

    const glm::vec2 cameraXZ(cameraPosition.x, cameraPosition.z);
    for (size_t index = 0; index < bucketTotal; ++index) {
        const GrassChunkBucket& bucket = resource.grassBuckets[index];
        if (bucket.count == 0) {
            continue;
        }

        const AABB worldBounds = bucket.localBounds.Transform(resource.model);
        const glm::vec2 closest = glm::clamp(
            cameraXZ,
            glm::vec2(worldBounds.min.x, worldBounds.min.z),
            glm::vec2(worldBounds.max.x, worldBounds.max.z));
        const float distance = glm::distance(cameraXZ, closest);
        if (distance > kGrassViewDistance ||
            !worldBounds.IsInsideFrustum(frustumPlanes)) {
            continue;
        }

        resource.grassVisibleBucketMask[index] = 1u;
        resource.grassCullCandidateBlades += bucket.count;
        if (resource.grassCullCandidates.size() >= kGrassBladeBucketListCap) {
            // 保留完整的可见 mask 供桶级 CPU 回退；叶片级列表超预算时
            // 由调用方安全回退到全量 GPU dispatch。
            resource.grassCullCoarseOverflow = true;
            continue;
        }
        resource.grassCullCandidates.push_back({
            bucket.firstInstance,
            bucket.count,
            worldBounds.min.y,
            worldBounds.max.y,
            distance,
            worldBounds.min.x,
            worldBounds.min.z,
            worldBounds.max.x,
            worldBounds.max.z});
    }

    std::sort(resource.grassCullCandidates.begin(),
              resource.grassCullCandidates.end(),
              [](const Resource::GrassCullCandidate& lhs,
                 const Resource::GrassCullCandidate& rhs) {
                  return lhs.distance > rhs.distance;
              });

    resource.grassCullCachedCameraPosition = cameraPosition;
    resource.grassCullCachedModel = resource.model;
    resource.grassCullCachedFrustumPlanes = frustumPlanes;
    resource.grassCullCacheValid = true;
    return false;
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
    // MIKAN_GRASS_COMPUTE=1 显式启用叶片级 GPU 剔除；未设置时使用上面的
    // 编译期默认值，设为 0 时强制关闭；关闭后保留 CPU 桶级粗筛 + MDI。
    static const bool bladeGpuCullEnabled = []{
        const char* env = std::getenv("MIKAN_GRASS_COMPUTE");
        return env != nullptr ? std::strcmp(env, "1") == 0
                              : kGrassBladeGpuCullDefaultEnabled;
    }();
    if (!bladeGpuCullEnabled) {
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
    // （readonly），5 = 上一帧 Hi-Z（combined image sampler）。
    VkDescriptorSetLayoutBinding bindings[7]{};
    for (uint32_t b = 0; b < 5; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[6] = bindings[5];
    bindings[6].binding = 6;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 7;
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
    poolSizes[0].descriptorCount = 512;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 256;   // Hi-Z + heightmap per view/frame
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 128;
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

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_GrassBladeCullDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_GrassBladeCullDescriptorLayout;
    for (int slot = 0; slot < kGrassCullViewSlots; ++slot) {
        VkDescriptorSet& descriptorSet = resource.grassBladeCullDescriptorSets[frame][slot];
        if (descriptorSet == VK_NULL_HANDLE &&
            vkAllocateDescriptorSets(g_Device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
            LOGE("[TerrainRenderer] grass blade cull descriptor set allocation failed (slot=%d)",
                 slot);
            descriptorSet = VK_NULL_HANDLE;
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
    for (int slot = 0; slot < kGrassCullViewSlots; ++slot) {
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t b = 0; b < 5; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = resource.grassBladeCullDescriptorSets[frame][slot];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &bufferInfos[b];
        }
        vkUpdateDescriptorSets(g_Device, 5, writes, 0, nullptr);
        const TextureInfo* height = g_TexturePool ? g_TexturePool->GetTexture(resource.heightmapKey) : nullptr;
        if (!height || height->imageView == VK_NULL_HANDLE) return false;
        VkDescriptorImageInfo heightInfo{};
        heightInfo.imageView = height->imageView;
        heightInfo.sampler = g_TexturePool->GetSampler(resource.heightmapKey);
        heightInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (heightInfo.sampler == VK_NULL_HANDLE) return false;
        VkWriteDescriptorSet heightWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        heightWrite.dstSet = resource.grassBladeCullDescriptorSets[frame][slot];
        heightWrite.dstBinding = 6;
        heightWrite.descriptorCount = 1;
        heightWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        heightWrite.pImageInfo = &heightInfo;
        vkUpdateDescriptorSets(g_Device, 1, &heightWrite, 0, nullptr);
    }
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
        return;   // 叶片级被禁用或初始化失败时，主 pass 回退 CPU 逐桶。
    }
    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    const glm::mat4 viewProj = proj * view;
    const uint64_t renderEpoch = g_SceneRenderer.GetRenderWorld().frameNumber;
    // SceneView visualizes the main camera's visible set when its frustum
    // culling is enabled. Depth, projection and history must use that same
    // camera; the editor camera is only used to rasterize the resulting set.
    int hizViewSlot = viewSlot;
    glm::mat4 hizCurrentViewProj = viewProj;
    glm::vec3 mainCullCameraPosition{};
    bool useMainCameraHiZ = false;
    bool useMainCameraCull = false;
    if (g_RunMode == RunMode::Editor) {
        const auto& world = g_SceneRenderer.GetRenderWorld();
        const auto camera = std::find_if(world.cameras.begin(), world.cameras.end(),
            [](const RenderCameraData& value) { return value.isMainCamera; });
        if (camera != world.cameras.end() &&
            (camera->enableFrustumCulling || g_SceneRenderer.IsGameGrassHiZCullingEnabled())) {
            glm::mat4 mainView, mainProj;
            const float aspect = static_cast<float>(g_GameRenderTarget.GetWidth()) /
                                 std::max(g_GameRenderTarget.GetHeight(), 1u);
            if (g_SceneRenderer.GetMainCameraMatrices(aspect, mainView, mainProj,
                                                     mainCullCameraPosition)) {
                hizViewSlot = 1;
                hizCurrentViewProj = mainProj * mainView;
                useMainCameraHiZ = viewSlot == 0;
                useMainCameraCull = true;
            }
        }
    }
    HiZComputeShader* grassHiZ = g_SceneRenderer.GetGrassHiZShader(hizViewSlot);
    if (!g_SceneRenderer.IsGameGrassHiZCullingEnabled()) {
        // Re-enabling must obtain a fresh main-camera depth frame first.
        m_GrassHiZHasPreviousView.fill(false);
    }
    RenderTarget& grassHiZTarget =
        (g_RunMode == RunMode::Editor && hizViewSlot == 0)
            ? g_SceneRenderTarget : g_GameRenderTarget;
    const uint64_t occluderRevision = GrassHiZOccluderRevision(g_SceneRenderer.GetRenderWorld());
    const bool terrainEdited = std::any_of(m_PreparedResources.begin(), m_PreparedResources.end(),
        [](const Resource* resource) { return resource != nullptr && resource->grassDirty; });
    const bool grassHiZViewStable = !terrainEdited && HiZHistory::CanReuse(
        hizCurrentViewProj, m_GrassHiZPreviousViewProj[static_cast<size_t>(hizViewSlot)],
        occluderRevision, m_GrassHiZOccluderRevision[static_cast<size_t>(hizViewSlot)],
        m_GrassHiZHasPreviousView[static_cast<size_t>(hizViewSlot)]);
    const bool grassHiZEnabled =
        g_SceneRenderer.IsGrassHiZCullingEnabled(hizViewSlot) &&
        grassHiZ != nullptr && grassHiZ->IsInitialized() &&
        grassHiZ->HasValidCullingData() && grassHiZ->GetCullingMipLevels() > 0 &&
        grassHiZViewStable;
    if (grassHiZEnabled) {
        static std::array<bool, kGrassCullViewSlots> s_loggedGrassHiZ{};
        if (!s_loggedGrassHiZ[static_cast<size_t>(viewSlot)]) {
            LOGI("[TerrainRenderer] grass Hi-Z culling active (viewSlot=%d, sourceSlot=%d, %ux%u, %u mips)",
                 viewSlot, hizViewSlot, grassHiZTarget.GetWidth(), grassHiZTarget.GetHeight(),
                 grassHiZ->GetCullingMipLevels());
            s_loggedGrassHiZ[static_cast<size_t>(viewSlot)] = true;
        }
    }
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

        // binding 5 必须始终绑定有效图像：Hi-Z 尚未有上一帧数据时绑定该
        // 视图的深度附件，但通过参数 w=0 禁止 shader 读取，避免空描述符。
        VkDescriptorImageInfo hizImageInfo{};
        hizImageInfo.sampler = grassHiZTarget.GetHiZSampler();
        hizImageInfo.imageView = grassHiZEnabled
            ? grassHiZ->GetHiZTextureViewForCulling()
            : grassHiZTarget.GetDepthImageView();
        hizImageInfo.imageLayout = grassHiZEnabled
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet hizWrite{};
        hizWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        hizWrite.dstSet = resource->grassBladeCullDescriptorSets[frame][viewSlot];
        hizWrite.dstBinding = 5;
        hizWrite.descriptorCount = 1;
        hizWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        hizWrite.pImageInfo = &hizImageInfo;
        vkUpdateDescriptorSets(g_Device, 1, &hizWrite, 0, nullptr);

        auto& cullViews = resource->grassGpuCullViews[frame];
        // 本帧第一次 dispatch 前清整帧记录：上一周期未被重新 dispatch 的段
        // 自动回落 CPU 路径，避免读到陈旧命令段。
        if (resource->grassGpuCullClearedFrame != renderEpoch) {
            for (auto& cullView : cullViews) {
                cullView = Resource::GrassGpuCullView{};
            }
            resource->grassGpuCullClearedFrame = renderEpoch;
        }

        // 剔除参考系：与 RenderGrass CPU 回退 / 桶级 GPU 路径同一规则——
        // 优先 Prepare 缓存（编辑器"主相机剔除"时即主相机视锥，与地形 chunk
        // 同源），缓存无效（首帧）时用当前视图矩阵现场提平面。
        const bool useCachedCull =
            resource->grassUseFrustumCulling && !IsProbeViewSlot(viewSlot) &&
            g_SceneRenderer.IsGrassFrustumCullingEnabled();
        const std::array<Plane, 6> cullPlanes = useCachedCull
            ? resource->grassFrustumPlanes
            : AABBUtils::ExtractFrustumPlanes(viewProj);
        const std::array<Plane, 6> effectiveCullPlanes =
            !g_SceneRenderer.IsGrassFrustumCullingEnabled() ? UnrestrictedGrassPlanes() : useMainCameraCull
            ? AABBUtils::ExtractFrustumPlanes(hizCurrentViewProj) : cullPlanes;
        const glm::vec3 camPos = useMainCameraCull ? mainCullCameraPosition : useCachedCull
            ? resource->grassCameraPosition
            : glm::vec3(glm::inverse(view)[3]);

        // ① 命令 SSBO 每帧 fill 清零（两视图段共用一张命令缓冲，逐视图 fill
        //   会把先 dispatch 的视图段原子计数抹零，故整帧只清一次）。
        if (resource->grassBladeCmdResetFrame != renderEpoch) {
            vkCmdFillBuffer(commandBuffer, resource->grassBladeCmdBuffers[frame].GetBuffer(),
                            0, static_cast<VkDeviceSize>(kGrassCullViewSlots) *
                                sizeof(VkDrawIndirectCommand), 0);
            resource->grassBladeCmdResetFrame = renderEpoch;
        }
        // ② 参数段：model + 6 平面 + 相机/视距 + 全局 Y 范围（局部空间）+ 叶
        //   保守半径 + 上一帧 Hi-Z 投影/尺寸。两视图各写各段。
        GrassBladeCullParams params{};
        params.model = resource->model;
        for (int p = 0; p < 6; ++p) {
            params.planes[p] = glm::vec4(effectiveCullPlanes[p].normal, effectiveCullPlanes[p].distance);
        }
        params.camAndDist = glm::vec4(camPos, kGrassViewDistance);
        params.heightRange = glm::vec4(resource->grassBladeMinY, resource->grassBladeMaxY,
                                       kGrassBladeCullRadius, 0.0f);
        params.bladeTerrain = glm::vec4(resource->settings.heightScale, resource->settings.heightOffset,
                                       resource->settings.worldSize.x, resource->settings.worldSize.y);
        params.bladeShape = glm::vec4(1.0f, 1.0f, 0.005f, 0.0f);
        params.hizViewProj = grassHiZEnabled
            ? HiZHistory::WithJitter(m_GrassHiZPreviousViewProj[static_cast<size_t>(hizViewSlot)],
                                    m_GrassHiZPreviousJitter[static_cast<size_t>(hizViewSlot)])
            : hizCurrentViewProj;
        params.hizParams = glm::uvec4(
            grassHiZTarget.GetWidth(), grassHiZTarget.GetHeight(),
            grassHiZEnabled ? grassHiZ->GetCullingMipLevels() : 0u,
            grassHiZEnabled ? 1u : 0u);
        static_assert(sizeof(params) == sizeof(cullViews[0].bladeCullKey),
                      "Update the shared grass key when the GPU parameter layout changes");
        const VkImageView cullDepthView = grassHiZEnabled ? hizImageInfo.imageView : VK_NULL_HANDLE;
        bool reusedResult = false;
        for (int sourceSlot = 0; sourceSlot < kGrassCullViewSlots; ++sourceSlot) {
            const auto& sourceView = cullViews[static_cast<size_t>(sourceSlot)];
            if (!sourceView.dispatched || sourceView.bladeResultSlot < 0 ||
                sourceView.renderEpoch != renderEpoch ||
                sourceView.bladeSourceCount != resource->grassInstanceCount ||
                sourceView.bladeHiZView != cullDepthView ||
                std::memcmp(sourceView.bladeCullKey.data(), &params, sizeof(params)) != 0) {
                continue;
            }
            auto sharedView = sourceView;
            sharedView.viewProj = viewProj; // Raster matrix stays specific to this viewport.
            cullViews[static_cast<size_t>(viewSlot)] = sharedView;
            reusedResult = true;
            if (std::getenv("MIKAN_GRASS_CULL_STATS")) {
                LOGI("[TerrainRenderer][GrassCullStats] blade result reused: viewSlot=%d resultSlot=%d",
                     viewSlot, sharedView.bladeResultSlot);
            }
            break;
        }
        if (reusedResult) continue;
        const bool grassCullCacheHit = EnsureGrassCullCache(*resource, camPos, effectiveCullPlanes);
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
        static std::vector<BucketListEntry> coarseList;   // 排序后的上传副本
        const auto& coarseBuckets = resource->grassCullCandidates;
        const bool coarseOverflow = resource->grassCullCoarseOverflow;
        const uint32_t coarseSrcBlades = resource->grassCullCandidateBlades;
        const bool useCoarse = !coarseOverflow && !coarseBuckets.empty();
        if (useCoarse) {
            // EnsureGrassCullCache 已按最近点距离降序稳定生成列表；相机不变
            // 时这里仅把历史结果编码到本帧槽位，不再重做桶 AABB 测试/排序。
            coarseList.clear();
            coarseList.reserve(coarseBuckets.size());
            for (const Resource::GrassCullCandidate& candidate : coarseBuckets) {
                coarseList.push_back({candidate.first, candidate.count,
                                      std::bit_cast<uint32_t>(candidate.yLo),
                                      std::bit_cast<uint32_t>(candidate.yHi),
                                      std::bit_cast<uint32_t>(candidate.minX),
                                      std::bit_cast<uint32_t>(candidate.minZ),
                                      std::bit_cast<uint32_t>(candidate.maxX),
                                      std::bit_cast<uint32_t>(candidate.maxZ)});
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
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &toCompute, 0, nullptr, 0, nullptr);

        // ⑤ dispatch：粗筛模式一工作组一桶（组数 = 幸存桶数，桶内 64 线程跨步）；
        //   全量回退一叶一调用。可见叶 atomicAdd 压缩进紧凑流 [viewSlot] 段。
        //   粗筛全灭（无溢出）时不 dispatch，命令段保持 fill 清零态 → 间接绘制
        //   instanceCount=0 自然空转。
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_GrassBladeCullPipeline);
        VkDescriptorSet bladeCullSet =
            resource->grassBladeCullDescriptorSets[frame][viewSlot];
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_GrassBladeCullPipelineLayout, 0, 1,
                                &bladeCullSet, 0, nullptr);
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
        cullViews[static_cast<size_t>(viewSlot)].bladeResultSlot = viewSlot;
        std::memcpy(cullViews[static_cast<size_t>(viewSlot)].bladeCullKey.data(), &params, sizeof(params));
        cullViews[static_cast<size_t>(viewSlot)].bladeSourceCount = resource->grassInstanceCount;
        cullViews[static_cast<size_t>(viewSlot)].bladeHiZView = cullDepthView;
        cullViews[static_cast<size_t>(viewSlot)].renderEpoch = renderEpoch;
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
                 grassCullCacheHit ? "cached-result" :
                     (useCachedCull ? "cached-ref" : "live"));
        }
    }
    if (!useMainCameraHiZ && grassHiZ != nullptr && grassHiZ->IsInitialized()) {
        // 当前 dispatch 使用上一帧矩阵；本帧对应 RT 在本函数返回后才会
        // 生成新的 Hi-Z，下一帧再消费，避免当前视图/当前深度错配。
        m_GrassHiZPreviousViewProj[static_cast<size_t>(viewSlot)] = viewProj;
        m_GrassHiZPreviousJitter[static_cast<size_t>(viewSlot)] = g_CurrentTAAJitter;
        m_GrassHiZOccluderRevision[static_cast<size_t>(viewSlot)] = occluderRevision;
        m_GrassHiZHasPreviousView[static_cast<size_t>(viewSlot)] = true;
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
    const uint64_t renderEpoch = g_SceneRenderer.GetRenderWorld().frameNumber;
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
        if (resource->grassGpuCullClearedFrame != renderEpoch) {
            for (auto& cullView : cullViews) {
                cullView = Resource::GrassGpuCullView{};
            }
            resource->grassGpuCullClearedFrame = renderEpoch;
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
        const bool useCachedCull =
            resource->grassUseFrustumCulling && !s_cullProbe && !IsProbeViewSlot(viewSlot) &&
            g_SceneRenderer.IsGrassFrustumCullingEnabled();
        const std::array<Plane, 6> cullPlanes = useCachedCull
            ? resource->grassFrustumPlanes
            : (g_SceneRenderer.IsGrassFrustumCullingEnabled()
                ? AABBUtils::ExtractFrustumPlanes(viewProj) : UnrestrictedGrassPlanes());
        const glm::vec3 camPos = useCachedCull
            ? resource->grassCameraPosition
            : glm::vec3(glm::inverse(viewProj)[3]);
        const bool grassCullCacheHit = EnsureGrassCullCache(*resource, camPos, cullPlanes);
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
                    const bool visible = i < resource->grassVisibleBucketMask.size() &&
                                         resource->grassVisibleBucketMask[i] != 0u;
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
                LOGI("[TerrainRenderer][GrassCullStats] gpu-cull slot=%d: compute dispatch groups=%u buckets=%u cam=(%.1f,%.1f,%.1f) cache=%s",
                     viewSlot, (bucketTotal + 63) / 64, bucketTotal,
                     camPos.x, camPos.y, camPos.z,
                     grassCullCacheHit ? "hit" : "miss");
            }
        } else {
            std::vector<VkDrawIndirectCommand> cpuCmds(bucketTotal);
            uint32_t statsVisibleBuckets = 0;
            uint32_t statsVisibleInstances = 0;
            for (uint32_t i = 0; i < bucketTotal; ++i) {
                const GrassChunkBucket& bucket = resource->grassBuckets[i];
                const bool visible = i < resource->grassVisibleBucketMask.size() &&
                                     resource->grassVisibleBucketMask[i] != 0u;
                if (visible) {
                    ++statsVisibleBuckets;
                    statsVisibleInstances += bucket.count;
                }
                cpuCmds[i].vertexCount = 10;
                cpuCmds[i].instanceCount = visible ? bucket.count : 0;
                cpuCmds[i].firstInstance = bucket.firstInstance;
            }
            if (std::getenv("MIKAN_GRASS_CULL_STATS")) {
                LOGI("[TerrainRenderer][GrassCullStats] gpu-cull slot=%d: buckets=%u visible=%u instances=%u cam=(%.1f,%.1f,%.1f) cache=%s",
                     viewSlot, bucketTotal, statsVisibleBuckets, statsVisibleInstances,
                     camPos.x, camPos.y, camPos.z,
                     grassCullCacheHit ? "hit" : "miss");
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
        cullViews[static_cast<size_t>(viewSlot)].renderEpoch = renderEpoch;
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
