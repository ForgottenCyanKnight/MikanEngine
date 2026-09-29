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

// Terrain height queries, raycasts, editor brushes, and map export.
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

bool TerrainRenderer::GetSubmergedDepth(ECS::Entity entity,
                                        const glm::vec3& worldPosition,
                                        float& outDepth) const {
    outDepth = 0.0f;
    const auto it = m_Resources.find(entity);
    if (it == m_Resources.end() || !it->second) {
        return false;
    }

    const Resource& resource = *it->second;
    if (!resource.settings.enabled || resource.heightmapCpu.empty() ||
        resource.waterCpu.empty() || resource.waterWidth < 2 || resource.waterHeight < 2 ||
        resource.waterCpu.size() < static_cast<size_t>(resource.waterWidth) * resource.waterHeight) {
        return false;
    }

    const glm::vec2 worldSize = glm::max(glm::abs(resource.settings.worldSize), glm::vec2(0.0001f));
    const glm::vec3 local = WorldToTerrainLocal(resource.model, worldPosition);
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) ||
        local.x < -worldSize.x * 0.5f || local.x > worldSize.x * 0.5f ||
        local.z < -worldSize.y * 0.5f || local.z > worldSize.y * 0.5f) {
        return false;
    }

    // Match terrain_water.vert's texel-center addressing and linear water-map
    // filtering (the CPU mirror is top-left row-major, while UV +Z is bottom).
    const float u = std::clamp(local.x / worldSize.x + 0.5f, 0.0f, 1.0f);
    const float v = std::clamp(local.z / worldSize.y + 0.5f, 0.0f, 1.0f);
    const float texelX = u * static_cast<float>(resource.waterWidth - 1u);
    const float texelY = (1.0f - v) * static_cast<float>(resource.waterHeight - 1u);
    const int maxX = static_cast<int>(resource.waterWidth) - 1;
    const int maxY = static_cast<int>(resource.waterHeight) - 1;
    const int x0 = std::clamp(static_cast<int>(std::floor(texelX)), 0, maxX);
    const int y0 = std::clamp(static_cast<int>(std::floor(texelY)), 0, maxY);
    const int x1 = std::min(x0 + 1, maxX);
    const int y1 = std::min(y0 + 1, maxY);
    const float fx = texelX - static_cast<float>(x0);
    const float fy = texelY - static_cast<float>(y0);
    const auto sampleWater = [&resource](int x, int y) {
        const size_t index = static_cast<size_t>(y) * resource.waterWidth +
                             static_cast<size_t>(x);
        return static_cast<float>(resource.waterCpu[index]) * (1.0f / 255.0f);
    };
    const float top = glm::mix(sampleWater(x0, y0), sampleWater(x1, y0), fx);
    const float bottom = glm::mix(sampleWater(x0, y1), sampleWater(x1, y1), fx);
    const float waterRaw = glm::mix(top, bottom, fy);
    if (waterRaw < 0.004f) {
        return false;
    }

    const HeightmapSampling heightmap{resource.heightmapCpu.data(),
        resource.heightmapWidth, resource.heightmapHeight, worldSize,
        resource.settings.heightScale, resource.settings.heightOffset};
    const float groundY = SampleLocalTerrainHeight(heightmap, local.x, local.z);
    const float t = std::clamp(waterRaw / 0.02f, 0.0f, 1.0f);
    const float shorelineBlend = t * t * (3.0f - 2.0f * t);
    const float waterSurfaceY = groundY + glm::mix(
        -0.04f, waterRaw * kTerrainWaterMaxDepth, shorelineBlend);
    if (local.y < groundY - 0.01f || local.y > waterSurfaceY) {
        return false;
    }

    const glm::vec3 surfaceWorld = TerrainLocalToWorld(
        resource.model, glm::vec3(local.x, waterSurfaceY, local.z));
    const float depth = surfaceWorld.y - worldPosition.y;
    if (!std::isfinite(depth) || depth <= 0.001f) {
        return false;
    }
    outDepth = depth;
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
    resource.terrainMdiBoundsDirty = true;

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
        resource.terrainMdiBoundsDirty = true;
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
