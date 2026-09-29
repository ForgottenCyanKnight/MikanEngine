#pragma once

#include "TerrainRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>

namespace TerrainRendererInternal {


constexpr size_t kInitialDescriptorSets = 512;

// 诊断开关（临时）：MIKAN_TERRAIN_DIAG=1 打印每个视图槽的地形 MDI 决策
// （Prepare 参考系 / RecordTerrainGpuCull 槽位与候选数 / 绘制时命中的槽位）。
// 用于排查「新增第三视图（反射探针，viewSlot=2）后主视图地形消失」这类
// 跨视图共享状态问题；默认关闭，不影响生产路径与性能。
inline bool TerrainDiagEnabled() {
    static const bool enabled = std::getenv("MIKAN_TERRAIN_DIAG") != nullptr;
    return enabled;
}

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

// ===== 地形 GPU MDI（每个原有 chunk 固定细分为 4x4 tile）=====
constexpr uint32_t kTerrainMdiTileSubdivision = 4;
// 既保证默认 chunkCount=8 的 32x32 tile 完整工作，也避免极端
// chunkCount=256 时创建百万 tile 的超大间接命令缓冲；超限资源保留 CPU fallback。
constexpr uint32_t kTerrainMdiMaxTiles = 65536;
// 仅扩大剔除包围盒，保持实际地形几何和 LOD 阈值不变，避免掠射角下
// GPU 细粒度视锥剔除误删贴近视锥边缘的山坡 tile。
constexpr float kTerrainMdiCullSafetyMargin = 1.0f;
constexpr VkDeviceSize kMaxUpdateBufferBytes = 65536;

struct TerrainMdiCullPush {
    glm::vec4 planes[6];        // xyz = normal, w = distance
    glm::vec4 cameraPosDist;    // xyz = 相机位置, w = 地形可见距离（<=0 = 不限）
    glm::uvec4 params;          // x = 候选数, y = 视图段, z = 段 capacity, w = tile 段基址
};
static_assert(sizeof(TerrainMdiCullPush) == 128,
              "terrain MDI cull push constant must be 128 bytes");

struct TerrainMdiCullViewGpu {
    glm::mat4 hizViewProj = glm::mat4(1.0f);
    glm::uvec4 hizParams = glm::uvec4(0u); // width, height, mip count, enabled
};
static_assert(sizeof(TerrainMdiCullViewGpu) == sizeof(float) * 20,
              "terrain MDI Hi-Z view data must stay 80 bytes");

// The hierarchy is generated from the previous GameRT, so the projection used
// to address it is only valid while the camera has not moved materially.  A
// stale reprojection is allowed to miss an occluder (less culling), but it must
// never remove a tile that is visible in the current view.
constexpr float kTerrainHiZViewProjStableEpsilon = 0.001f;

inline float TerrainHiZViewProjDelta(const glm::mat4& lhs, const glm::mat4& rhs) {
    float maxDelta = 0.0f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            maxDelta = std::max(maxDelta,
                                std::abs(lhs[column][row] - rhs[column][row]));
        }
    }
    return maxDelta;
}

inline bool SameVec3(const glm::vec3& lhs, const glm::vec3& rhs) {
    for (int component = 0; component < 3; ++component) {
        if (lhs[component] != rhs[component]) {
            return false;
        }
    }
    return true;
}

inline bool SameMat4(const glm::mat4& lhs, const glm::mat4& rhs) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (lhs[column][row] != rhs[column][row]) {
                return false;
            }
        }
    }
    return true;
}

inline bool SameFrustum(const std::array<Plane, 6>& lhs,
                 const std::array<Plane, 6>& rhs) {
    for (size_t plane = 0; plane < lhs.size(); ++plane) {
        if (!(lhs[plane] == rhs[plane])) {
            return false;
        }
    }
    return true;
}

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
inline const bool kGrassSegmentLodEnabled = []{
    const char* env = std::getenv("MIKAN_GRASS_LOD");
    return !(env != nullptr && env[0] == '0');
}();
// 叶片级剔除的叶级保守半径（XZ 方向）：叶高 0.42-1.02m + 风摆余量 + 地形局部
// 起伏。Y 方向由 FinalizeGrassBuckets 扫高度镜像的全局 [minY, maxY] 兜底。
constexpr float kGrassBladeCullRadius = 1.5f;
// 叶片级 GPU 剔除默认启用；需要采集 CPU 粗筛基线时设置
// MIKAN_GRASS_COMPUTE=0，或将此默认值临时改为 false。
constexpr bool kGrassBladeGpuCullDefaultEnabled = true;
// CPU 粗筛桶列表容量上限（65536 × 16B = 1MB hostVisible）：覆盖 subdiv=8
// 的 16384 桶全可见情形。上传按 64KB 分段（vkCmdUpdateBuffer 单次数据上限）。
// 超上限回退全量 dispatch 模式（shader info.z=1）。
constexpr uint32_t kGrassBladeBucketListCap = 65536;
// vkCmdUpdateBuffer 单次调用最大条目数（2048 × 32B = 64KB）。
constexpr uint32_t kGrassBladeBucketUploadChunk = 2048;

// g_PhysicalDevice 由 Core/VulkanContext.h 声明（本文件已包含）。

// params SSBO 与 grass_blade_cull.comp 的 ParamsBuf（std430）逐字段对齐：
// mat4=64B + vec4[6]=96B + vec4=16B + vec4=16B + mat4=64B
// + uvec4=16B = 272B。
struct GrassBladeCullParams {
    glm::mat4 model;
    glm::vec4 planes[6];
    glm::vec4 camAndDist;
    glm::vec4 heightRange;
    glm::mat4 hizViewProj;
    glm::uvec4 hizParams;
};
static_assert(sizeof(GrassBladeCullParams) == 272,
              "GrassBladeCullParams must match grass_blade_cull.comp std430 layout (272B)");

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

} // namespace TerrainRendererInternal
