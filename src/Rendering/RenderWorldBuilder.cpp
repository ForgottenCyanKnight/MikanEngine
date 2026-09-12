#include "Rendering/RenderWorldBuilder.h"

#include "Rendering/SceneCollector.h"
#include "ECS/Components.h"
#include "ECS/SceneECS.h"

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/quaternion.hpp>

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

bool RenderWorld::Validate(std::string* error) const
{
    if (error != nullptr) error->clear();
    const auto fail = [error](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    const auto describeEntity = [](ECS::Entity entity) {
        return std::to_string(entity);
    };

    if (entities.size() != indexByEntity.size()) {
        return fail("entity index size does not match entity record count");
    }

    std::unordered_set<ECS::Entity> entityIds;
    entityIds.reserve(entities.size());
    for (size_t index = 0; index < entities.size(); ++index) {
        const RenderWorldEntity& entity = entities[index];
        if (entity.entity == ECS::INVALID_ENTITY) {
            return fail("entity record contains INVALID_ENTITY");
        }
        if (!entityIds.insert(entity.entity).second) {
            return fail("duplicate entity record: " + describeEntity(entity.entity));
        }
        const auto indexIt = indexByEntity.find(entity.entity);
        if (indexIt == indexByEntity.end() || indexIt->second != index) {
            return fail("entity index points to the wrong record: " + describeEntity(entity.entity));
        }
    }

    if (hierarchyEntities.size() != entities.size()) {
        return fail("hierarchy/entity record counts differ");
    }
    std::unordered_set<ECS::Entity> hierarchyIds;
    hierarchyIds.reserve(hierarchyEntities.size());
    for (const ECS::Entity entity : hierarchyEntities) {
        if (Find(entity) == nullptr) {
            return fail("hierarchy references an unknown entity: " + describeEntity(entity));
        }
        if (!hierarchyIds.insert(entity).second) {
            return fail("duplicate hierarchy entity: " + describeEntity(entity));
        }
    }
    for (const ECS::Entity entity : entityIds) {
        if (hierarchyIds.find(entity) == hierarchyIds.end()) {
            return fail("entity record is missing from hierarchy: " + describeEntity(entity));
        }
    }

    std::unordered_set<ECS::Entity> rootIds;
    rootIds.reserve(rootEntities.size());
    for (const ECS::Entity root : rootEntities) {
        const RenderWorldEntity* rootData = Find(root);
        if (rootData == nullptr) {
            return fail("root references an unknown entity: " + describeEntity(root));
        }
        if (!rootIds.insert(root).second) {
            return fail("duplicate root entity: " + describeEntity(root));
        }
        if (rootData->parent != ECS::INVALID_ENTITY) {
            return fail("root has a parent: " + describeEntity(root));
        }
    }
    if (selectedEntity != ECS::INVALID_ENTITY && Find(selectedEntity) == nullptr) {
        return fail("selected entity is not present in the snapshot");
    }

    for (const RenderWorldEntity& entity : entities) {
        if (entity.parent == entity.entity) {
            return fail("entity is its own parent: " + describeEntity(entity.entity));
        }
        if (entity.parent != ECS::INVALID_ENTITY) {
            const RenderWorldEntity* parent = Find(entity.parent);
            if (parent == nullptr) {
                return fail("entity parent is not present: " + describeEntity(entity.entity));
            }
            if (std::find(parent->children.begin(), parent->children.end(), entity.entity) == parent->children.end()) {
                return fail("parent/child link is not reciprocal: " + describeEntity(entity.entity));
            }
        }

        std::unordered_set<ECS::Entity> childIds;
        childIds.reserve(entity.children.size());
        for (const ECS::Entity child : entity.children) {
            if (child == ECS::INVALID_ENTITY || child == entity.entity) {
                return fail("invalid child link on entity: " + describeEntity(entity.entity));
            }
            const RenderWorldEntity* childData = Find(child);
            if (childData == nullptr) {
                return fail("child is not present: " + describeEntity(child));
            }
            if (childData->parent != entity.entity) {
                return fail("child/parent link is not reciprocal: " + describeEntity(child));
            }
            if (!childIds.insert(child).second) {
                return fail("duplicate child link on entity: " + describeEntity(entity.entity));
            }
        }
    }

    std::unordered_set<ECS::Entity> modelGroupEntities;
    for (const RenderModelGroup& group : modelGroups) {
        if (group.rendererKey.empty() || group.modelPath.empty()) {
            return fail("model group has an empty renderer key or model path");
        }
        const bool keyMatchesPath =
            group.rendererKey == group.modelPath ||
            group.rendererKey.rfind(group.modelPath + "#entity:", 0) == 0;
        if (!keyMatchesPath) {
            return fail("model group renderer key does not match its model path");
        }
        for (const ECS::Entity entityId : group.entities) {
            const RenderWorldEntity* entity = Find(entityId);
            if (entity == nullptr || !entity->hasTransform || !entity->visible ||
                !entity->hasMesh || !entity->hasRenderFlags ||
                (entity->mesh.type != RenderMeshType::Model &&
                 entity->mesh.type != RenderMeshType::Plane) ||
                entity->mesh.modelPath != group.modelPath) {
                return fail("model group references an incompatible entity: " + describeEntity(entityId));
            }
            if (!modelGroupEntities.insert(entityId).second) {
                return fail("entity occurs more than once in model groups: " + describeEntity(entityId));
            }
        }
    }

    std::unordered_set<ECS::Entity> voxGroupEntities;
    for (const RenderVoxGroup& group : voxGroups) {
        if (group.voxPath.empty()) return fail("voxel group has an empty path");
        for (const ECS::Entity entityId : group.entities) {
            const RenderWorldEntity* entity = Find(entityId);
            if (entity == nullptr || !entity->hasTransform || !entity->visible ||
                !entity->hasVoxel || entity->voxel.voxPath != group.voxPath) {
                return fail("voxel group references an incompatible entity: " + describeEntity(entityId));
            }
            if (!voxGroupEntities.insert(entityId).second) {
                return fail("entity occurs more than once in voxel groups: " + describeEntity(entityId));
            }
        }
    }

    for (const RenderCameraData& camera : cameras) {
        const RenderWorldEntity* entity = Find(camera.entity);
        if (entity == nullptr || !entity->hasTransform || !entity->hasCamera ||
            entity->camera.entity != camera.entity) {
            return fail("camera list references an incompatible entity");
        }
    }
    for (const RenderLightData& light : lights) {
        const RenderWorldEntity* entity = Find(light.entity);
        if (entity == nullptr || !entity->hasTransform || !entity->hasLight ||
            entity->light.entity != light.entity) {
            return fail("light list references an incompatible entity");
        }
    }
    for (const RenderTerrainData& terrain : terrains) {
        const RenderWorldEntity* entity = Find(terrain.entity);
        if (entity == nullptr || !entity->hasTransform || !entity->hasTerrain ||
            entity->terrain.entity != terrain.entity) {
            return fail("terrain list references an incompatible entity");
        }
    }
    for (const RenderWaterData& water : waters) {
        const RenderWorldEntity* entity = Find(water.entity);
        if (entity == nullptr || !entity->hasTransform || !entity->hasWater ||
            entity->water.entity != water.entity) {
            return fail("water list references an incompatible entity");
        }
    }
    for (const RenderSkyboxData& skybox : skyboxes) {
        const RenderWorldEntity* entity = Find(skybox.entity);
        if (entity == nullptr || !entity->hasSkybox ||
            entity->skybox.entity != skybox.entity) {
            return fail("skybox list references an incompatible entity");
        }
    }
    for (const RenderCloudData& cloud : clouds) {
        const RenderWorldEntity* entity = Find(cloud.entity);
        if (entity == nullptr || !entity->hasCloud ||
            entity->cloud.entity != cloud.entity) {
            return fail("cloud list references an incompatible entity");
        }
    }

    return true;
}

namespace {

uint64_t g_renderWorldExtractionSerial = 0;

RenderLightType ToRenderLightType(ECS::LightComponent::Type type)
{
    switch (type) {
    case ECS::LightComponent::Type::Point: return RenderLightType::Point;
    case ECS::LightComponent::Type::Spot: return RenderLightType::Spot;
    case ECS::LightComponent::Type::Directional:
    default: return RenderLightType::Directional;
    }
}

RenderSpriteType ToRenderSpriteType(ECS::Sprite2DComponent::Type type)
{
    switch (type) {
    case ECS::Sprite2DComponent::Type::Sprite: return RenderSpriteType::Sprite;
    case ECS::Sprite2DComponent::Type::Button: return RenderSpriteType::Button;
    case ECS::Sprite2DComponent::Type::Rect:
    default: return RenderSpriteType::Rect;
    }
}

RenderTextMode ToRenderTextMode(ECS::TextComponent::RenderMode mode)
{
    switch (mode) {
    case ECS::TextComponent::RenderMode::Bitmap: return RenderTextMode::Bitmap;
    case ECS::TextComponent::RenderMode::Sdf: return RenderTextMode::Sdf;
    case ECS::TextComponent::RenderMode::Msdf:
    default: return RenderTextMode::Msdf;
    }
}

RenderVmdTarget ToRenderVmdTarget(ECS::VmdTarget target)
{
    switch (target) {
    case ECS::VmdTarget::Model: return RenderVmdTarget::Model;
    case ECS::VmdTarget::Camera: return RenderVmdTarget::Camera;
    case ECS::VmdTarget::Auto:
    default: return RenderVmdTarget::Auto;
    }
}

RenderColliderType ToRenderColliderType(ECS::ColliderComponent::Type type)
{
    switch (type) {
    case ECS::ColliderComponent::Type::Sphere: return RenderColliderType::Sphere;
    case ECS::ColliderComponent::Type::Capsule: return RenderColliderType::Capsule;
    case ECS::ColliderComponent::Type::Box:
    default: return RenderColliderType::Box;
    }
}

RenderRigidBodyType ToRenderRigidBodyType(ECS::RigidBodyComponent::Type type)
{
    switch (type) {
    case ECS::RigidBodyComponent::Type::Static: return RenderRigidBodyType::Static;
    case ECS::RigidBodyComponent::Type::Kinematic: return RenderRigidBodyType::Kinematic;
    case ECS::RigidBodyComponent::Type::Dynamic:
    default: return RenderRigidBodyType::Dynamic;
    }
}

RenderRigidBodyShapeType ToRenderRigidBodyShapeType(ECS::RigidBodyComponent::ShapeType type)
{
    switch (type) {
    case ECS::RigidBodyComponent::ShapeType::Sphere: return RenderRigidBodyShapeType::Sphere;
    case ECS::RigidBodyComponent::ShapeType::Capsule: return RenderRigidBodyShapeType::Capsule;
    case ECS::RigidBodyComponent::ShapeType::OBB: return RenderRigidBodyShapeType::OBB;
    case ECS::RigidBodyComponent::ShapeType::Mesh: return RenderRigidBodyShapeType::Mesh;
    case ECS::RigidBodyComponent::ShapeType::Box:
    default: return RenderRigidBodyShapeType::Box;
    }
}

void CopyMaterial(const ECS::MaterialComponent& source, RenderMaterialData& target)
{
    target.albedoPath = source.albedoPath;
    target.normalPath = source.normalPath;
    target.roughnessPath = source.roughnessPath;
    target.metallicPath = source.metallicPath;
    target.aoPath = source.aoPath;
    target.emissivePath = source.emissivePath;
    target.albedoSamplerType = source.albedoSamplerType;
    target.normalSamplerType = source.normalSamplerType;
    target.roughnessSamplerType = source.roughnessSamplerType;
    target.metallicSamplerType = source.metallicSamplerType;
    target.aoSamplerType = source.aoSamplerType;
    target.emissiveSamplerType = source.emissiveSamplerType;
    target.albedoColor = source.albedoColor;
    target.metallic = source.metallic;
    target.roughness = source.roughness;
    target.ao = source.ao;
    target.emissiveIntensity = source.emissiveIntensity;
    target.useAlbedoTexture = source.useAlbedoTexture;
    target.useNormalTexture = source.useNormalTexture;
    target.useRoughnessTexture = source.useRoughnessTexture;
    target.useMetallicTexture = source.useMetallicTexture;
    target.useAOTexture = source.useAOTexture;
    target.useEmissiveTexture = source.useEmissiveTexture;
}

void CopyTerrain(const ECS::TerrainComponent& source, RenderTerrainData& target)
{
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
}

void CopyWater(const ECS::WaterComponent& source, RenderWaterData& target)
{
    target.enabled = source.enabled;
    target.size = source.size;
    target.surfaceOffset = source.surfaceOffset;
    target.depth = source.depth;
    target.buoyancy = source.buoyancy;
    target.drag = source.drag;
    target.color = source.color;
    target.roughness = source.roughness;
    target.affectPlayersOnly = source.affectPlayersOnly;
}

void CopyCloud(const ECS::CloudVolumeComponent& source, RenderCloudData& target)
{
    target.enabled = source.enabled;
    target.coverage = source.coverage;
    target.density = source.density;
    target.baseAltitudeKm = source.baseAltitudeKm;
    target.thicknessKm = source.thicknessKm;
    target.noiseScale = source.noiseScale;
    target.detailErosion = source.detailErosion;
    target.detailScale = source.detailScale;
    target.lightAbsorption = source.lightAbsorption;
    target.multipleScattering = source.multipleScattering;
    target.multipleScatteringBuild = source.multipleScatteringBuild;
    target.multipleScatteringBoundary = source.multipleScatteringBoundary;
    target.multipleScatteringCompress = source.multipleScatteringCompress;
    target.noiseOffsetKm = source.noiseOffsetKm;
    target.windSpeedKmPerSecond = source.windSpeedKmPerSecond;
    target.windDirectionXZ = source.windDirectionXZ;
    target.highCloudEnabled = source.highCloudEnabled;
    target.highCloudCoverage = source.highCloudCoverage;
    target.highCloudDensity = source.highCloudDensity;
    target.highCloudAltitudeKm = source.highCloudAltitudeKm;
    target.highCloudThicknessKm = source.highCloudThicknessKm;
    target.highCloudScale = source.highCloudScale;
    target.highCloudDetail = source.highCloudDetail;
    target.highCloudBrightness = source.highCloudBrightness;
    target.highCloudWindSpeedKmPerSecond = source.highCloudWindSpeedKmPerSecond;
    target.highCloudWindDirectionXZ = source.highCloudWindDirectionXZ;
}

} // namespace

void SceneCollector::BuildRenderWorldFromECS(RenderWorld& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();

    const auto& hierarchy = scene.GetHierarchyEntities();
    out.BeginBuild(hierarchy.size());
    out.frameNumber = ++g_renderWorldExtractionSerial;
    out.entitySetVersion = scene.GetEntitySetVersion();
    out.selectedEntity = scene.GetSelectedEntity();
    const auto& roots = scene.GetRootEntities();
    out.rootEntities.reserve(roots.size());
    for (const ECS::Entity root : roots) {
        if (coordinator.IsAlive(root)) out.rootEntities.push_back(root);
    }
    out.hierarchyEntities.reserve(hierarchy.size());
    for (const ECS::Entity entity : hierarchy) {
        if (coordinator.IsAlive(entity)) out.hierarchyEntities.push_back(entity);
    }
    out.cameras.reserve(hierarchy.size());
    out.lights.reserve(hierarchy.size());
    out.terrains.reserve(hierarchy.size());
    out.waters.reserve(hierarchy.size());

    std::unordered_map<std::string, size_t> modelGroupIndices;
    std::unordered_map<std::string, size_t> voxGroupIndices;

    size_t entityWriteIndex = 0;
    for (const ECS::Entity entity : out.hierarchyEntities) {
        RenderWorldEntity& snapshot = out.entities[entityWriteIndex++];
        snapshot.ResetForBuild();
        snapshot.entity = entity;
        const ECS::Entity parent = scene.GetParent(entity);
        snapshot.parent = (parent != ECS::INVALID_ENTITY && coordinator.IsAlive(parent))
            ? parent
            : ECS::INVALID_ENTITY;
        for (const ECS::Entity child : scene.GetChildren(entity)) {
            if (coordinator.IsAlive(child)) snapshot.children.push_back(child);
        }
        snapshot.visible = scene.IsVisible(entity);

        const ECS::TransformComponent* transformComponent = nullptr;
        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            const auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
            transformComponent = &transform;
            snapshot.hasTransform = true;
            snapshot.transform.position = transform.position;
            snapshot.transform.rotation = transform.rotation;
            snapshot.transform.scale = transform.scale;
            snapshot.transform.worldMatrix = scene.GetWorldMatrix(entity);
            snapshot.transform.eulerAngles = glm::degrees(glm::eulerAngles(transform.rotation));
        }

        if (coordinator.HasComponent<ECS::MeshComponent>(entity)) {
            const auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            snapshot.hasMesh = true;
            snapshot.mesh.type = static_cast<RenderMeshType>(static_cast<uint8_t>(mesh.type));
            snapshot.mesh.modelPath = mesh.modelPath;
        }
        if (coordinator.HasComponent<ECS::MaterialComponent>(entity)) {
            snapshot.hasMaterial = true;
            CopyMaterial(coordinator.GetComponent<ECS::MaterialComponent>(entity), snapshot.material);
        }
        if (coordinator.HasComponent<ECS::RenderComponent>(entity)) {
            const auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
            snapshot.hasRenderFlags = true;
            snapshot.render.visible = render.visible;
            snapshot.render.castShadow = render.castShadow;
            snapshot.render.receiveShadow = render.receiveShadow;
            snapshot.render.showAABB = render.showAABB;
            snapshot.render.showOBB = render.showOBB;
            snapshot.render.doubleSided = render.doubleSided;
            snapshot.render.wireframe = render.wireframe;
        }
        if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
            const auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
            snapshot.hasVoxel = true;
            snapshot.voxel.voxPath = vox.voxPath;
            snapshot.voxel.loaded = vox.loaded;
            snapshot.voxel.isStatic = vox.isStatic;
        }
        if (coordinator.HasComponent<ECS::AnimatorComponent>(entity)) {
            const auto& animator = coordinator.GetComponent<ECS::AnimatorComponent>(entity);
            snapshot.hasAnimator = true;
            snapshot.animator.clipIndex = animator.clipIndex;
            snapshot.animator.speed = animator.speed;
            snapshot.animator.loop = animator.loop;
            snapshot.animator.playing = animator.playing;
            snapshot.animator.time = animator.time;
        }
        if (coordinator.HasComponent<ECS::VmdPlayerComponent>(entity)) {
            const auto& vmd = coordinator.GetComponent<ECS::VmdPlayerComponent>(entity);
            snapshot.hasVmdPlayer = true;
            snapshot.vmd.target = ToRenderVmdTarget(vmd.target);
            snapshot.vmd.enabled = vmd.enabled;
            snapshot.vmd.hasMotion = !vmd.motionPath.empty();
        }

        if (coordinator.HasComponent<ECS::CameraComponent>(entity) && snapshot.hasTransform) {
            const auto& camera = coordinator.GetComponent<ECS::CameraComponent>(entity);
            snapshot.hasCamera = true;
            snapshot.camera.entity = entity;
            snapshot.camera.fov = camera.fov;
            snapshot.camera.nearPlane = camera.nearPlane;
            snapshot.camera.farPlane = camera.farPlane;
            snapshot.camera.isMainCamera = camera.isMainCamera;
            snapshot.camera.isOrthographic = camera.isOrthographic;
            snapshot.camera.orthographicSize = camera.orthographicSize;
            snapshot.camera.enableFrustumCulling = camera.enableFrustumCulling;
            snapshot.camera.showFrustumWireframe = camera.showFrustumWireframe;
            snapshot.camera.useSubMeshCulling = camera.useSubMeshCulling;
            snapshot.camera.showBVHWireframe = camera.showBVHWireframe;
            snapshot.camera.showCollisionWireframe = camera.showCollisionWireframe;
            snapshot.camera.position = transformComponent->position;
            snapshot.camera.rotation = transformComponent->rotation;
            snapshot.camera.postProcessChain = camera.postProcessChain;
            out.cameras.push_back(snapshot.camera);
        }
        if (coordinator.HasComponent<ECS::Camera2DComponent>(entity)) {
            snapshot.hasCamera2D = true;
            snapshot.camera2DEnabled = coordinator.GetComponent<ECS::Camera2DComponent>(entity).enabled;
        }
        if (coordinator.HasComponent<ECS::LightComponent>(entity) && snapshot.hasTransform) {
            const auto& light = coordinator.GetComponent<ECS::LightComponent>(entity);
            snapshot.hasLight = true;
            snapshot.light.entity = entity;
            snapshot.light.type = ToRenderLightType(light.type);
            snapshot.light.color = light.color;
            snapshot.light.intensity = light.intensity;
            snapshot.light.range = light.range;
            snapshot.light.spotAngle = light.spotAngle;
            snapshot.light.castShadow = light.castShadow;
            snapshot.light.position = transformComponent->position;
            snapshot.light.rotation = transformComponent->rotation;
            out.lights.push_back(snapshot.light);
        }
        if (coordinator.HasComponent<ECS::Canvas2DComponent>(entity)) {
            const auto& canvas = coordinator.GetComponent<ECS::Canvas2DComponent>(entity);
            snapshot.hasCanvas = true;
            snapshot.canvas.width = canvas.width;
            snapshot.canvas.height = canvas.height;
            snapshot.canvas.stretchToViewport = canvas.stretchToViewport;
            snapshot.canvas.layer = canvas.layer;
        }
        if (coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
            const auto& sprite = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
            snapshot.hasSprite = true;
            snapshot.sprite.type = ToRenderSpriteType(sprite.type);
            snapshot.sprite.visible = sprite.visible;
            snapshot.sprite.isUI = sprite.isUI;
            snapshot.sprite.width = sprite.width;
            snapshot.sprite.height = sprite.height;
            snapshot.sprite.uv0 = sprite.uv0;
            snapshot.sprite.uv1 = sprite.uv1;
            snapshot.sprite.color = sprite.color;
            snapshot.sprite.texture = sprite.texture;
            snapshot.sprite.layer = sprite.layer;
            snapshot.sprite.anchorMin = sprite.anchorMin;
            snapshot.sprite.anchorMax = sprite.anchorMax;
            snapshot.sprite.hovered = sprite.hovered;
            snapshot.sprite.label = sprite.label;
            snapshot.sprite.labelFontSize = sprite.labelFontSize;
            snapshot.sprite.labelColor = sprite.labelColor;
        }
        if (coordinator.HasComponent<ECS::ButtonComponent>(entity)) {
            const auto& button = coordinator.GetComponent<ECS::ButtonComponent>(entity);
            snapshot.hasButton = true;
            snapshot.button.visible = button.visible;
            snapshot.button.isUI = button.isUI;
            snapshot.button.width = button.width;
            snapshot.button.height = button.height;
            snapshot.button.fillColor = button.fillColor;
            snapshot.button.hoverColor = button.hoverColor;
            snapshot.button.textColor = button.textColor;
            snapshot.button.text = button.text;
            snapshot.button.fontSize = button.fontSize;
            snapshot.button.layer = button.layer;
            snapshot.button.hovered = button.hovered;
        }
        if (coordinator.HasComponent<ECS::TextComponent>(entity)) {
            const auto& text = coordinator.GetComponent<ECS::TextComponent>(entity);
            snapshot.hasText = true;
            snapshot.text.text = text.text;
            snapshot.text.visible = text.visible;
            snapshot.text.isUI = text.isUI;
            snapshot.text.fontSize = text.fontSize;
            snapshot.text.color = text.color;
            snapshot.text.layer = text.layer;
            snapshot.text.renderMode = ToRenderTextMode(text.renderMode);
            snapshot.text.measuredWidth = text.measuredWidth;
            snapshot.text.measuredHeight = text.measuredHeight;
        }
        if (coordinator.HasComponent<ECS::Slice9Component>(entity)) {
            const auto& slice = coordinator.GetComponent<ECS::Slice9Component>(entity);
            snapshot.hasSlice9 = true;
            snapshot.slice9.texture = slice.texture;
            snapshot.slice9.visible = slice.visible;
            snapshot.slice9.isUI = slice.isUI;
            snapshot.slice9.width = slice.width;
            snapshot.slice9.height = slice.height;
            snapshot.slice9.border = slice.border;
            snapshot.slice9.uv0 = slice.uv0;
            snapshot.slice9.uv1 = slice.uv1;
            snapshot.slice9.color = slice.color;
            snapshot.slice9.layer = slice.layer;
            snapshot.slice9.texWidth = slice.texWidth;
            snapshot.slice9.texHeight = slice.texHeight;
        }
        snapshot.hasTilemap = coordinator.HasComponent<ECS::TilemapComponent>(entity);
        if (coordinator.HasComponent<ECS::TerrainComponent>(entity) && snapshot.hasTransform) {
            snapshot.hasTerrain = true;
            snapshot.terrain.entity = entity;
            CopyTerrain(coordinator.GetComponent<ECS::TerrainComponent>(entity), snapshot.terrain);
            if (snapshot.visible && snapshot.terrain.enabled) out.terrains.push_back(snapshot.terrain);
        }
        if (coordinator.HasComponent<ECS::WaterComponent>(entity) && snapshot.hasTransform) {
            snapshot.hasWater = true;
            snapshot.water.entity = entity;
            CopyWater(coordinator.GetComponent<ECS::WaterComponent>(entity), snapshot.water);
            if (snapshot.visible && snapshot.water.enabled) out.waters.push_back(snapshot.water);
        }
        if (coordinator.HasComponent<ECS::SkyboxComponent>(entity)) {
            const auto& skybox = coordinator.GetComponent<ECS::SkyboxComponent>(entity);
            snapshot.hasSkybox = true;
            snapshot.skybox.entity = entity;
            snapshot.skybox.enabled = skybox.enabled;
            snapshot.skybox.textureName = skybox.textureName;
            snapshot.skybox.tint = skybox.tint;
            snapshot.skybox.intensity = skybox.intensity;
            out.skyboxes.push_back(snapshot.skybox);
        }
        if (coordinator.HasComponent<ECS::CloudVolumeComponent>(entity)) {
            const auto& cloud = coordinator.GetComponent<ECS::CloudVolumeComponent>(entity);
            snapshot.hasCloud = true;
            snapshot.cloud.entity = entity;
            CopyCloud(cloud, snapshot.cloud);
            out.clouds.push_back(snapshot.cloud);
        }
        if (coordinator.HasComponent<ECS::ColliderComponent>(entity)) {
            const auto& collider = coordinator.GetComponent<ECS::ColliderComponent>(entity);
            snapshot.hasCollider = true;
            snapshot.collider.type = ToRenderColliderType(collider.type);
            snapshot.collider.offset = collider.offset;
            snapshot.collider.size = collider.size;
            snapshot.collider.isTrigger = collider.isTrigger;
            snapshot.collider.useOBB = collider.useOBB;
        }
        if (coordinator.HasComponent<ECS::RigidBodyComponent>(entity)) {
            const auto& rigidBody = coordinator.GetComponent<ECS::RigidBodyComponent>(entity);
            snapshot.hasRigidBody = true;
            snapshot.rigidBody.type = ToRenderRigidBodyType(rigidBody.type);
            snapshot.rigidBody.isTrigger = rigidBody.isTrigger;
            snapshot.rigidBody.shapeType = ToRenderRigidBodyShapeType(rigidBody.shapeType);
            snapshot.rigidBody.size = rigidBody.size;
            snapshot.rigidBody.offset = rigidBody.offset;
            snapshot.rigidBody.generatePerSubmesh = rigidBody.generatePerSubmesh;
        }

        const bool hasVisibleRender = snapshot.visible && snapshot.hasTransform;
        if (hasVisibleRender && snapshot.hasMesh && snapshot.hasRenderFlags &&
            (snapshot.mesh.type == RenderMeshType::Model || snapshot.mesh.type == RenderMeshType::Plane) &&
            !snapshot.mesh.modelPath.empty()) {
            const bool perEntityAnimation = snapshot.hasAnimator || snapshot.hasVmdPlayer;
            const std::string key = snapshot.mesh.modelPath +
                (perEntityAnimation ? "#entity:" + std::to_string(entity) : "");
            auto [it, inserted] = modelGroupIndices.emplace(key, out.modelGroups.size());
            if (inserted) {
                RenderModelGroup group;
                group.rendererKey = key;
                group.modelPath = snapshot.mesh.modelPath;
                out.modelGroups.push_back(std::move(group));
            }
            out.modelGroups[it->second].entities.push_back(entity);
        }
        if (hasVisibleRender && snapshot.hasVoxel && !snapshot.voxel.voxPath.empty()) {
            auto [it, inserted] = voxGroupIndices.emplace(snapshot.voxel.voxPath, out.voxGroups.size());
            if (inserted) {
                RenderVoxGroup group;
                group.voxPath = snapshot.voxel.voxPath;
                out.voxGroups.push_back(std::move(group));
            }
            out.voxGroups[it->second].entities.push_back(entity);
        }
    }

    out.entities.resize(entityWriteIndex);
    out.particles = ParticleSystem::GetInstance().GetRenderInstances();
    out.RebuildIndex();
}

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

void RenderWorldBuilder::Build(RenderWorld& out, RenderWorldBuildStats* stats)
{
    const BuildClock::time_point buildStart = BuildClock::now();

    // SceneCollector remains the compatibility seam for the existing ECS
    // traversal.  All callers outside this module use this builder entry
    // point, so timing and future parallel extraction stay centralized here.
    SceneCollector::BuildRenderWorldFromECS(out);
    const BuildClock::time_point buildEnd = BuildClock::now();

    RenderWorldBuildStats result;
    FillStats(out, result);
    result.buildMilliseconds =
        std::chrono::duration<double, std::milli>(buildEnd - buildStart).count();

    if (IsRenderWorldValidationEnabled()) {
        result.validationChecked = true;
        std::string validationError;
        result.invariantsValid = out.Validate(&validationError);
        result.invariantErrorCount = result.invariantsValid ? 0u : 1u;
        if (!result.invariantsValid) {
            std::fprintf(stderr,
                "[RenderWorld][Validation] frame=%llu error=%s\n",
                static_cast<unsigned long long>(out.frameNumber),
                validationError.c_str());
            if (IsRenderWorldValidationStrictEnabled()) std::abort();
        }
    }

    if (stats != nullptr) *stats = result;
}
