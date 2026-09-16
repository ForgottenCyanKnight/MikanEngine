#include "Rendering/RenderWorldBuilder.h"

#include "Rendering/SceneCollector.h"
#include "Core/Log.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using BuildClock = std::chrono::steady_clock;

template <typename T>
void AddVectorStorage(const std::vector<T>& values, uint64_t& bytes)
{
    bytes += static_cast<uint64_t>(values.capacity()) * sizeof(T);
}

void AddStringStorage(const std::string& value, uint64_t& bytes)
{
    // capacity() is the owned character storage and intentionally excludes
    // the string object itself, which is already covered by its containing
    // renderer-facing record.
    bytes += static_cast<uint64_t>(value.capacity()) * sizeof(char);
}

uint64_t EstimateContainerBytes(const RenderWorld& world)
{
    uint64_t bytes = sizeof(RenderWorld);
    AddVectorStorage(world.rootEntities, bytes);
    AddVectorStorage(world.hierarchyEntities, bytes);
    AddVectorStorage(world.entities, bytes);
    AddVectorStorage(world.modelGroups, bytes);
    AddVectorStorage(world.voxGroups, bytes);
    AddVectorStorage(world.modelEntities, bytes);
    AddVectorStorage(world.voxEntities, bytes);
    AddVectorStorage(world.cullingEntities, bytes);
    AddVectorStorage(world.cameras, bytes);
    AddVectorStorage(world.lights, bytes);
    AddVectorStorage(world.terrains, bytes);
    AddVectorStorage(world.waters, bytes);
    AddVectorStorage(world.skyboxes, bytes);
    AddVectorStorage(world.clouds, bytes);
    AddVectorStorage(world.particles, bytes);

    for (const RenderWorldEntity& entity : world.entities) {
        AddVectorStorage(entity.children, bytes);
        AddStringStorage(entity.mesh.modelPath, bytes);
        AddStringStorage(entity.material.albedoPath, bytes);
        AddStringStorage(entity.material.normalPath, bytes);
        AddStringStorage(entity.material.roughnessPath, bytes);
        AddStringStorage(entity.material.metallicPath, bytes);
        AddStringStorage(entity.material.aoPath, bytes);
        AddStringStorage(entity.material.emissivePath, bytes);
        AddStringStorage(entity.voxel.voxPath, bytes);
        AddStringStorage(entity.camera.postProcessChain, bytes);
        AddStringStorage(entity.sprite.texture, bytes);
        AddStringStorage(entity.sprite.label, bytes);
        AddStringStorage(entity.button.text, bytes);
        AddStringStorage(entity.text.text, bytes);
        AddStringStorage(entity.slice9.texture, bytes);
        AddStringStorage(entity.terrain.heightmapPath, bytes);
        AddStringStorage(entity.terrain.layer0Path, bytes);
        AddStringStorage(entity.terrain.layer1Path, bytes);
        AddStringStorage(entity.terrain.layer2Path, bytes);
        AddStringStorage(entity.terrain.layer3Path, bytes);
        AddStringStorage(entity.terrain.controlMapPath, bytes);
        AddStringStorage(entity.skybox.textureName, bytes);
    }

    for (const RenderModelGroup& group : world.modelGroups) {
        AddStringStorage(group.rendererKey, bytes);
        AddStringStorage(group.modelPath, bytes);
        AddVectorStorage(group.entities, bytes);
    }
    for (const RenderVoxGroup& group : world.voxGroups) {
        AddStringStorage(group.voxPath, bytes);
        AddVectorStorage(group.entities, bytes);
    }
    for (const RenderCameraData& camera : world.cameras) {
        AddStringStorage(camera.postProcessChain, bytes);
    }
    for (const RenderTerrainData& terrain : world.terrains) {
        AddStringStorage(terrain.heightmapPath, bytes);
        AddStringStorage(terrain.layer0Path, bytes);
        AddStringStorage(terrain.layer1Path, bytes);
        AddStringStorage(terrain.layer2Path, bytes);
        AddStringStorage(terrain.layer3Path, bytes);
        AddStringStorage(terrain.controlMapPath, bytes);
    }
    for (const RenderSkyboxData& skybox : world.skyboxes) {
        AddStringStorage(skybox.textureName, bytes);
    }

    return bytes;
}

void FillStats(const RenderWorld& world, RenderWorldBuildStats& stats)
{
    stats.frameNumber = world.frameNumber;
    stats.entitySetVersion = world.entitySetVersion;
    stats.incrementalCaptureUsed = world.incrementalCaptureUsed;
    stats.reusedTransformCount = world.reusedTransformCount;
    stats.recomputedTransformCount = world.recomputedTransformCount;
    stats.reusedComponentCount = world.reusedComponentCount;
    stats.recomputedComponentCount = world.recomputedComponentCount;
    stats.hierarchyEntityCount = static_cast<uint32_t>(world.hierarchyEntities.size());
    stats.entityCount = static_cast<uint32_t>(world.entities.size());
    stats.modelGroupCount = static_cast<uint32_t>(world.modelGroups.size());
    stats.voxGroupCount = static_cast<uint32_t>(world.voxGroups.size());
    stats.cameraCount = static_cast<uint32_t>(world.cameras.size());
    stats.lightCount = static_cast<uint32_t>(world.lights.size());
    stats.terrainCount = static_cast<uint32_t>(world.terrains.size());
    stats.waterCount = static_cast<uint32_t>(world.waters.size());
    stats.skyboxCount = static_cast<uint32_t>(world.skyboxes.size());
    stats.cloudCount = static_cast<uint32_t>(world.clouds.size());
    stats.particleCount = static_cast<uint32_t>(world.particles.size());

    for (const RenderWorldEntity& entity : world.entities) {
        if (entity.visible) ++stats.visibleEntityCount;
    }
    for (const RenderModelGroup& group : world.modelGroups) {
        stats.modelEntityCount += static_cast<uint32_t>(group.entities.size());
    }
    for (const RenderVoxGroup& group : world.voxGroups) {
        stats.voxEntityCount += static_cast<uint32_t>(group.entities.size());
    }

    stats.estimatedContainerBytes = EstimateContainerBytes(world);
}

} // namespace

bool IsRenderWorldValidationEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MIKAN_RENDERWORLD_VALIDATE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool IsRenderWorldValidationStrictEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MIKAN_RENDERWORLD_VALIDATE_STRICT");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

void RenderWorldBuilder::Capture(RenderWorld& out)
{
    SceneCollector::CaptureRenderWorldFromECS(out);
}

void RenderWorldBuilder::Finalize(RenderWorld& out)
{
    std::vector<RenderWorldFinalizePartition> partitions(1);
    FinalizeRange(out, 0, out.entities.size(), partitions[0]);
    MergeFinalizedPartitions(out, partitions);
}

void RenderWorldBuilder::CollectStats(const RenderWorld& world,
                                      RenderWorldBuildStats& stats,
                                      double buildMilliseconds)
{
    RenderWorldBuildStats result;
    FillStats(world, result);
    result.buildMilliseconds = buildMilliseconds;

    if (IsRenderWorldValidationEnabled()) {
        result.validationChecked = true;
        std::string validationError;
        result.invariantsValid = world.Validate(&validationError);
        result.invariantErrorCount = result.invariantsValid ? 0u : 1u;
        if (!result.invariantsValid) {
            LOGE("[RenderWorld][Validation] frame=%llu error=%s",
                static_cast<unsigned long long>(world.frameNumber),
                validationError.c_str());
            if (IsRenderWorldValidationStrictEnabled()) std::abort();
        }
    }

    stats = result;
}

void RenderWorldBuilder::Build(RenderWorld& out, RenderWorldBuildStats* stats)
{
    const BuildClock::time_point buildStart = BuildClock::now();

    // Capture is the only phase that reads live ECS.  Finalize consumes the
    // exclusive staging buffer and is deliberately isolated so a later job
    // scheduler can run it off the main thread without changing the public
    // Build contract.
    Capture(out);
    const BuildClock::time_point captureEnd = BuildClock::now();
    Finalize(out);
    const BuildClock::time_point finalizeEnd = BuildClock::now();
    const double captureMilliseconds =
        std::chrono::duration<double, std::milli>(captureEnd - buildStart).count();
    const double finalizeMilliseconds =
        std::chrono::duration<double, std::milli>(finalizeEnd - captureEnd).count();

    RenderWorldBuildStats result;
    CollectStats(out, result,
        captureMilliseconds + finalizeMilliseconds);
    result.captureMilliseconds = captureMilliseconds;
    result.finalizeMilliseconds = finalizeMilliseconds;

    if (stats != nullptr) *stats = result;
}
