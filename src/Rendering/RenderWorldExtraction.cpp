#include "Rendering/SceneCollector.h"
#include "ECS/Components.h"
#include "ECS/SceneECS.h"
#include "Rendering/RenderWorldBuilder.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/quaternion.hpp>

namespace {

uint64_t g_renderWorldExtractionSerial = 0;

class CaptureRevisionBuilder {
public:
    template <typename T>
    void AddScalar(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            m_hash ^= bytes[index];
            m_hash *= 1099511628211ull;
        }
    }

    void AddString(const std::string& value)
    {
        AddScalar(static_cast<uint64_t>(value.size()));
        for (const unsigned char character : value) AddScalar(character);
    }

    void AddVec2(const glm::vec2& value)
    {
        AddScalar(value.x);
        AddScalar(value.y);
    }

    void AddVec3(const glm::vec3& value)
    {
        AddScalar(value.x);
        AddScalar(value.y);
        AddScalar(value.z);
    }

    void AddVec4(const glm::vec4& value)
    {
        AddScalar(value.x);
        AddScalar(value.y);
        AddScalar(value.z);
        AddScalar(value.w);
    }

    void AddQuat(const glm::quat& value)
    {
        AddScalar(value.x);
        AddScalar(value.y);
        AddScalar(value.z);
        AddScalar(value.w);
    }

    uint64_t Finish() const
    {
        return m_hash == 0 ? 1 : m_hash;
    }

private:
    uint64_t m_hash = 1469598103934665603ull;
};

uint64_t HashMesh(const ECS::MeshComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(static_cast<uint8_t>(value.type));
    hash.AddString(value.modelPath);
    return hash.Finish();
}

uint64_t HashMaterial(const ECS::MaterialComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddString(value.albedoPath);
    hash.AddString(value.normalPath);
    hash.AddString(value.roughnessPath);
    hash.AddString(value.metallicPath);
    hash.AddString(value.aoPath);
    hash.AddString(value.emissivePath);
    hash.AddScalar(value.albedoSamplerType);
    hash.AddScalar(value.normalSamplerType);
    hash.AddScalar(value.roughnessSamplerType);
    hash.AddScalar(value.metallicSamplerType);
    hash.AddScalar(value.aoSamplerType);
    hash.AddScalar(value.emissiveSamplerType);
    hash.AddVec3(value.albedoColor);
    hash.AddScalar(value.metallic);
    hash.AddScalar(value.roughness);
    hash.AddScalar(value.ao);
    hash.AddScalar(value.emissiveIntensity);
    hash.AddScalar(value.useAlbedoTexture);
    hash.AddScalar(value.useNormalTexture);
    hash.AddScalar(value.useRoughnessTexture);
    hash.AddScalar(value.useMetallicTexture);
    hash.AddScalar(value.useAOTexture);
    hash.AddScalar(value.useEmissiveTexture);
    return hash.Finish();
}

uint64_t HashRenderFlags(const ECS::RenderComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.visible);
    hash.AddScalar(value.castShadow);
    hash.AddScalar(value.receiveShadow);
    hash.AddScalar(value.showAABB);
    hash.AddScalar(value.showOBB);
    hash.AddScalar(value.doubleSided);
    hash.AddScalar(value.wireframe);
    return hash.Finish();
}

uint64_t HashVoxel(const ECS::VoxModelComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddString(value.voxPath);
    hash.AddScalar(value.loaded);
    hash.AddScalar(value.isStatic);
    return hash.Finish();
}

uint64_t HashAnimator(const ECS::AnimatorComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.clipIndex);
    hash.AddScalar(value.speed);
    hash.AddScalar(value.loop);
    hash.AddScalar(value.playing);
    hash.AddScalar(value.time);
    return hash.Finish();
}

uint64_t HashVmd(const ECS::VmdPlayerComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(static_cast<uint8_t>(value.target));
    hash.AddScalar(value.enabled);
    hash.AddScalar(!value.motionPath.empty());
    return hash.Finish();
}

void HashTransformPositionRotation(CaptureRevisionBuilder& hash,
                                    const ECS::TransformComponent* transform)
{
    hash.AddScalar(transform != nullptr);
    if (transform != nullptr) {
        hash.AddVec3(transform->position);
        hash.AddQuat(transform->rotation);
    }
}

uint64_t HashCamera(const ECS::CameraComponent& value,
                    const ECS::TransformComponent* transform)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.fov);
    hash.AddScalar(value.nearPlane);
    hash.AddScalar(value.farPlane);
    hash.AddScalar(value.isMainCamera);
    hash.AddScalar(value.isOrthographic);
    hash.AddScalar(value.orthographicSize);
    hash.AddScalar(value.enableFrustumCulling);
    hash.AddScalar(value.showFrustumWireframe);
    hash.AddScalar(value.useSubMeshCulling);
    hash.AddScalar(value.showBVHWireframe);
    hash.AddScalar(value.showCollisionWireframe);
    hash.AddString(value.postProcessChain);
    HashTransformPositionRotation(hash, transform);
    return hash.Finish();
}

uint64_t HashCamera2D(const ECS::Camera2DComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.enabled);
    return hash.Finish();
}

uint64_t HashLight(const ECS::LightComponent& value,
                   const ECS::TransformComponent* transform)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(static_cast<uint8_t>(value.type));
    hash.AddVec3(value.color);
    hash.AddScalar(value.intensity);
    hash.AddScalar(value.range);
    hash.AddScalar(value.spotAngle);
    hash.AddScalar(value.castShadow);
    HashTransformPositionRotation(hash, transform);
    return hash.Finish();
}

uint64_t HashCanvas(const ECS::Canvas2DComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.width);
    hash.AddScalar(value.height);
    hash.AddScalar(value.stretchToViewport);
    hash.AddScalar(value.layer);
    return hash.Finish();
}

uint64_t HashSprite(const ECS::Sprite2DComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(static_cast<uint8_t>(value.type));
    hash.AddScalar(value.visible);
    hash.AddScalar(value.isUI);
    hash.AddScalar(value.width);
    hash.AddScalar(value.height);
    hash.AddVec2(value.uv0);
    hash.AddVec2(value.uv1);
    hash.AddVec4(value.color);
    hash.AddString(value.texture);
    hash.AddScalar(value.layer);
    hash.AddVec2(value.anchorMin);
    hash.AddVec2(value.anchorMax);
    hash.AddScalar(value.hovered);
    hash.AddString(value.label);
    hash.AddScalar(value.labelFontSize);
    hash.AddVec4(value.labelColor);
    return hash.Finish();
}

uint64_t HashButton(const ECS::ButtonComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.visible);
    hash.AddScalar(value.isUI);
    hash.AddScalar(value.width);
    hash.AddScalar(value.height);
    hash.AddVec4(value.fillColor);
    hash.AddVec4(value.hoverColor);
    hash.AddVec4(value.textColor);
    hash.AddString(value.text);
    hash.AddScalar(value.fontSize);
    hash.AddScalar(value.layer);
    hash.AddScalar(value.hovered);
    return hash.Finish();
}

uint64_t HashText(const ECS::TextComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddString(value.text);
    hash.AddScalar(value.visible);
    hash.AddScalar(value.isUI);
    hash.AddScalar(value.fontSize);
    hash.AddVec4(value.color);
    hash.AddScalar(value.layer);
    hash.AddScalar(static_cast<uint8_t>(value.renderMode));
    hash.AddVec2(value.anchorMin);
    hash.AddVec2(value.anchorMax);
    hash.AddVec2(value.pivot);
    hash.AddScalar(value.measuredWidth);
    hash.AddScalar(value.measuredHeight);
    return hash.Finish();
}

uint64_t HashSlice9(const ECS::Slice9Component& value)
{
    CaptureRevisionBuilder hash;
    hash.AddString(value.texture);
    hash.AddScalar(value.visible);
    hash.AddScalar(value.isUI);
    hash.AddScalar(value.width);
    hash.AddScalar(value.height);
    hash.AddVec4(value.border);
    hash.AddVec2(value.uv0);
    hash.AddVec2(value.uv1);
    hash.AddVec4(value.color);
    hash.AddScalar(value.layer);
    hash.AddScalar(value.texWidth);
    hash.AddScalar(value.texHeight);
    return hash.Finish();
}

uint64_t HashTerrain(const ECS::TerrainComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.enabled);
    hash.AddString(value.heightmapPath);
    hash.AddVec2(value.worldSize);
    hash.AddScalar(value.heightScale);
    hash.AddScalar(value.heightOffset);
    hash.AddScalar(value.chunkCount);
    hash.AddScalar(value.patchResolution);
    hash.AddScalar(value.viewDistance);
    hash.AddScalar(value.lod0Distance);
    hash.AddScalar(value.lod1Distance);
    hash.AddScalar(value.maxLod);
    hash.AddScalar(value.wireframe);
    hash.AddScalar(value.materialTiling);
    hash.AddScalar(value.blendSharpness);
    hash.AddString(value.layer0Path);
    hash.AddString(value.layer1Path);
    hash.AddString(value.layer2Path);
    hash.AddString(value.layer3Path);
    hash.AddString(value.controlMapPath);
    return hash.Finish();
}

uint64_t HashWater(const ECS::WaterComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.enabled);
    hash.AddVec2(value.size);
    hash.AddScalar(value.surfaceOffset);
    hash.AddScalar(value.depth);
    hash.AddScalar(value.buoyancy);
    hash.AddScalar(value.drag);
    hash.AddVec3(value.color);
    hash.AddScalar(value.roughness);
    hash.AddScalar(value.affectPlayersOnly);
    return hash.Finish();
}

uint64_t HashSkybox(const ECS::SkyboxComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.enabled);
    hash.AddString(value.textureName);
    hash.AddVec3(value.tint);
    hash.AddScalar(value.intensity);
    return hash.Finish();
}

uint64_t HashCloud(const ECS::CloudVolumeComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(value.enabled);
    hash.AddScalar(value.coverage);
    hash.AddScalar(value.density);
    hash.AddScalar(value.baseAltitudeKm);
    hash.AddScalar(value.thicknessKm);
    hash.AddScalar(value.noiseScale);
    hash.AddScalar(value.detailErosion);
    hash.AddScalar(value.detailScale);
    hash.AddScalar(value.lightAbsorption);
    hash.AddScalar(value.multipleScattering);
    hash.AddScalar(value.multipleScatteringBuild);
    hash.AddScalar(value.multipleScatteringBoundary);
    hash.AddScalar(value.multipleScatteringCompress);
    hash.AddVec3(value.noiseOffsetKm);
    hash.AddScalar(value.windSpeedKmPerSecond);
    hash.AddVec2(value.windDirectionXZ);
    hash.AddScalar(value.highCloudEnabled);
    hash.AddScalar(value.highCloudCoverage);
    hash.AddScalar(value.highCloudDensity);
    hash.AddScalar(value.highCloudAltitudeKm);
    hash.AddScalar(value.highCloudThicknessKm);
    hash.AddScalar(value.highCloudScale);
    hash.AddScalar(value.highCloudDetail);
    hash.AddScalar(value.highCloudBrightness);
    hash.AddScalar(value.highCloudWindSpeedKmPerSecond);
    hash.AddVec2(value.highCloudWindDirectionXZ);
    return hash.Finish();
}

uint64_t HashCollider(const ECS::ColliderComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(static_cast<uint8_t>(value.type));
    hash.AddVec3(value.offset);
    hash.AddVec3(value.size);
    hash.AddScalar(value.isTrigger);
    hash.AddScalar(value.useOBB);
    return hash.Finish();
}

uint64_t HashRigidBody(const ECS::RigidBodyComponent& value)
{
    CaptureRevisionBuilder hash;
    hash.AddScalar(static_cast<uint8_t>(value.type));
    hash.AddScalar(value.isTrigger);
    hash.AddScalar(static_cast<uint8_t>(value.shapeType));
    hash.AddVec3(value.size);
    hash.AddVec3(value.offset);
    hash.AddScalar(value.generatePerSubmesh);
    return hash.Finish();
}

bool PrepareComponentCapture(RenderWorld& out,
                             RenderWorldEntity& snapshot,
                             RenderWorldCaptureComponent component,
                             bool present,
                             uint64_t revision)
{
    const bool reuse = out.incrementalCaptureUsed &&
        snapshot.HasMatchingCaptureRevision(component, present, revision);
    if (present) {
        if (reuse) ++out.reusedComponentCount;
        else ++out.recomputedComponentCount;
    }
    snapshot.SetCaptureRevision(component, present, revision);
    return reuse;
}

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

void SceneCollector::CaptureRenderWorldFromECS(RenderWorld& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();

    const auto& hierarchy = scene.GetHierarchyEntities();
    const uint32_t sceneVersion = scene.GetEntitySetVersion();
    const bool canReuseTransformCache =
        out.entitySetVersion == sceneVersion &&
        out.entities.size() == hierarchy.size() &&
        out.hierarchyEntities == hierarchy;
    out.BeginBuild(hierarchy.size(), canReuseTransformCache);
    out.frameNumber = ++g_renderWorldExtractionSerial;
    out.entitySetVersion = sceneVersion;
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

    std::unordered_map<ECS::Entity, size_t> captureIndices;
    captureIndices.reserve(out.hierarchyEntities.size());
    size_t entityWriteIndex = 0;
    for (const ECS::Entity entity : out.hierarchyEntities) {
        RenderWorldEntity& snapshot = out.entities[entityWriteIndex++];
        const bool previousHasTransform = snapshot.capturedTransformHasTransform;
        const ECS::Entity previousParent = snapshot.parent;
        const uint32_t previousLocalTransformVersion =
            snapshot.capturedTransformLocalVersion;
        const uint64_t previousParentWorldVersion =
            snapshot.capturedTransformParentWorldVersion;
        const uint64_t previousWorldVersion =
            snapshot.capturedTransformWorldVersion;
        const ECS::Entity previousCapturedParent = snapshot.capturedTransformParent;
        const bool previousParentHasTransform =
            snapshot.capturedTransformParentHasTransform;

        snapshot.ResetForBuild(out.incrementalCaptureUsed);
        snapshot.entity = entity;
        const ECS::Entity parent = scene.GetParent(entity);
        snapshot.parent = (parent != ECS::INVALID_ENTITY && coordinator.IsAlive(parent))
            ? parent
            : ECS::INVALID_ENTITY;
        captureIndices[entity] = entityWriteIndex - 1;
        for (const ECS::Entity child : scene.GetChildren(entity)) {
            if (coordinator.IsAlive(child)) snapshot.children.push_back(child);
        }
        snapshot.visible = scene.IsVisible(entity);

        const ECS::TransformComponent* transformComponent = nullptr;
        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            const auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
            transformComponent = &transform;
            snapshot.hasTransform = true;
            bool parentHasTransform = false;
            uint64_t parentWorldVersion = 0;
            if (snapshot.parent != ECS::INVALID_ENTITY) {
                const auto parentIndex = captureIndices.find(snapshot.parent);
                if (parentIndex != captureIndices.end()) {
                    const RenderWorldEntity& parentSnapshot =
                        out.entities[parentIndex->second];
                    parentHasTransform = parentSnapshot.hasTransform;
                    if (parentHasTransform) {
                        parentWorldVersion = parentSnapshot.capturedTransformWorldVersion;
                    }
                } else if (coordinator.HasComponent<ECS::TransformComponent>(snapshot.parent)) {
                    // The hierarchy cache is normally parent-before-child.  If
                    // a caller supplies an unusual hierarchy order, preserve
                    // correctness by falling back to SceneECS' cached matrix.
                    parentHasTransform = true;
                    scene.GetWorldMatrix(snapshot.parent);
                    parentWorldVersion = coordinator.GetComponent<ECS::TransformComponent>(
                        snapshot.parent).worldVersion;
                }
            }

            const uint64_t currentWorldVersion =
                parentWorldVersion + static_cast<uint64_t>(transform.localVersion);
            const bool previousParentMatches = previousParent == snapshot.parent;
            const bool capturedParentMatches = previousCapturedParent == snapshot.parent;
            const bool localVersionMatches =
                previousLocalTransformVersion == transform.localVersion;
            const bool parentWorldVersionMatches =
                previousParentWorldVersion == parentWorldVersion;
            const bool parentTransformPresenceMatches =
                previousParentHasTransform == parentHasTransform;
            const bool worldVersionMatches = previousWorldVersion == currentWorldVersion;
            const bool reuseTransform =
                out.incrementalCaptureUsed && previousHasTransform &&
                previousParentMatches && capturedParentMatches &&
                localVersionMatches && parentWorldVersionMatches &&
                parentTransformPresenceMatches && worldVersionMatches;
            if (reuseTransform) {
                ++out.reusedTransformCount;
            } else {
                snapshot.transform.position = transform.position;
                snapshot.transform.rotation = transform.rotation;
                snapshot.transform.scale = transform.scale;
                snapshot.transform.worldMatrix = scene.GetWorldMatrix(entity);
                snapshot.transform.eulerAngles = glm::degrees(glm::eulerAngles(transform.rotation));
                ++out.recomputedTransformCount;
            }
            snapshot.capturedTransformLocalVersion = transform.localVersion;
            snapshot.capturedTransformParentWorldVersion = parentWorldVersion;
            snapshot.capturedTransformWorldVersion = reuseTransform
                ? currentWorldVersion
                : transform.worldVersion;
            snapshot.capturedTransformParent = snapshot.parent;
            snapshot.capturedTransformParentHasTransform = parentHasTransform;
            snapshot.capturedTransformHasTransform = true;
        } else {
            snapshot.hasTransform = false;
            snapshot.transform = RenderTransformData{};
            snapshot.capturedTransformLocalVersion = 0;
            snapshot.capturedTransformParentWorldVersion = 0;
            snapshot.capturedTransformWorldVersion = 0;
            snapshot.capturedTransformParent = ECS::INVALID_ENTITY;
            snapshot.capturedTransformParentHasTransform = false;
            snapshot.capturedTransformHasTransform = false;
        }

        const bool hasMesh = coordinator.HasComponent<ECS::MeshComponent>(entity);
        const uint64_t meshRevision = hasMesh
            ? HashMesh(coordinator.GetComponent<ECS::MeshComponent>(entity)) : 0;
        const bool reuseMesh = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Mesh, hasMesh, meshRevision);
        snapshot.hasMesh = hasMesh;
        if (hasMesh && !reuseMesh) {
            const auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            snapshot.mesh.type = static_cast<RenderMeshType>(static_cast<uint8_t>(mesh.type));
            snapshot.mesh.modelPath = mesh.modelPath;
        }

        const bool hasMaterial = coordinator.HasComponent<ECS::MaterialComponent>(entity);
        const uint64_t materialRevision = hasMaterial
            ? HashMaterial(coordinator.GetComponent<ECS::MaterialComponent>(entity)) : 0;
        const bool reuseMaterial = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Material, hasMaterial, materialRevision);
        snapshot.hasMaterial = hasMaterial;
        if (hasMaterial && !reuseMaterial) {
            CopyMaterial(coordinator.GetComponent<ECS::MaterialComponent>(entity), snapshot.material);
        }

        const bool hasRenderFlags = coordinator.HasComponent<ECS::RenderComponent>(entity);
        const uint64_t renderFlagsRevision = hasRenderFlags
            ? HashRenderFlags(coordinator.GetComponent<ECS::RenderComponent>(entity)) : 0;
        const bool reuseRenderFlags = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::RenderFlags,
            hasRenderFlags, renderFlagsRevision);
        snapshot.hasRenderFlags = hasRenderFlags;
        if (hasRenderFlags && !reuseRenderFlags) {
            const auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
            snapshot.render.visible = render.visible;
            snapshot.render.castShadow = render.castShadow;
            snapshot.render.receiveShadow = render.receiveShadow;
            snapshot.render.showAABB = render.showAABB;
            snapshot.render.showOBB = render.showOBB;
            snapshot.render.doubleSided = render.doubleSided;
            snapshot.render.wireframe = render.wireframe;
        }

        const bool hasVoxel = coordinator.HasComponent<ECS::VoxModelComponent>(entity);
        const uint64_t voxelRevision = hasVoxel
            ? HashVoxel(coordinator.GetComponent<ECS::VoxModelComponent>(entity)) : 0;
        const bool reuseVoxel = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Voxel, hasVoxel, voxelRevision);
        snapshot.hasVoxel = hasVoxel;
        if (hasVoxel && !reuseVoxel) {
            const auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
            snapshot.voxel.voxPath = vox.voxPath;
            snapshot.voxel.loaded = vox.loaded;
            snapshot.voxel.isStatic = vox.isStatic;
        }

        const bool hasAnimator = coordinator.HasComponent<ECS::AnimatorComponent>(entity);
        const uint64_t animatorRevision = hasAnimator
            ? HashAnimator(coordinator.GetComponent<ECS::AnimatorComponent>(entity)) : 0;
        const bool reuseAnimator = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Animator, hasAnimator, animatorRevision);
        snapshot.hasAnimator = hasAnimator;
        if (hasAnimator && !reuseAnimator) {
            const auto& animator = coordinator.GetComponent<ECS::AnimatorComponent>(entity);
            snapshot.animator.clipIndex = animator.clipIndex;
            snapshot.animator.speed = animator.speed;
            snapshot.animator.loop = animator.loop;
            snapshot.animator.playing = animator.playing;
            snapshot.animator.time = animator.time;
        }

        const bool hasVmdPlayer = coordinator.HasComponent<ECS::VmdPlayerComponent>(entity);
        const uint64_t vmdRevision = hasVmdPlayer
            ? HashVmd(coordinator.GetComponent<ECS::VmdPlayerComponent>(entity)) : 0;
        const bool reuseVmd = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Vmd, hasVmdPlayer, vmdRevision);
        snapshot.hasVmdPlayer = hasVmdPlayer;
        if (hasVmdPlayer && !reuseVmd) {
            const auto& vmd = coordinator.GetComponent<ECS::VmdPlayerComponent>(entity);
            snapshot.vmd.target = ToRenderVmdTarget(vmd.target);
            snapshot.vmd.enabled = vmd.enabled;
            snapshot.vmd.hasMotion = !vmd.motionPath.empty();
        }

        const bool hasCamera = coordinator.HasComponent<ECS::CameraComponent>(entity) &&
            snapshot.hasTransform;
        const uint64_t cameraRevision = hasCamera
            ? HashCamera(coordinator.GetComponent<ECS::CameraComponent>(entity), transformComponent)
            : 0;
        const bool reuseCamera = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Camera, hasCamera, cameraRevision);
        snapshot.hasCamera = hasCamera;
        if (hasCamera) {
            if (!reuseCamera) {
                const auto& camera = coordinator.GetComponent<ECS::CameraComponent>(entity);
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
            }
            out.cameras.push_back(snapshot.camera);
        }

        const bool hasCamera2D = coordinator.HasComponent<ECS::Camera2DComponent>(entity);
        const uint64_t camera2DRevision = hasCamera2D
            ? HashCamera2D(coordinator.GetComponent<ECS::Camera2DComponent>(entity)) : 0;
        const bool reuseCamera2D = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Camera2D, hasCamera2D, camera2DRevision);
        snapshot.hasCamera2D = hasCamera2D;
        if (hasCamera2D && !reuseCamera2D) {
            snapshot.camera2DEnabled = coordinator.GetComponent<ECS::Camera2DComponent>(entity).enabled;
        }

        const bool hasLight = coordinator.HasComponent<ECS::LightComponent>(entity) &&
            snapshot.hasTransform;
        const uint64_t lightRevision = hasLight
            ? HashLight(coordinator.GetComponent<ECS::LightComponent>(entity), transformComponent)
            : 0;
        const bool reuseLight = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Light, hasLight, lightRevision);
        snapshot.hasLight = hasLight;
        if (hasLight) {
            if (!reuseLight) {
                const auto& light = coordinator.GetComponent<ECS::LightComponent>(entity);
                snapshot.light.entity = entity;
                snapshot.light.type = ToRenderLightType(light.type);
                snapshot.light.color = light.color;
                snapshot.light.intensity = light.intensity;
                snapshot.light.range = light.range;
                snapshot.light.spotAngle = light.spotAngle;
                snapshot.light.castShadow = light.castShadow;
                snapshot.light.position = transformComponent->position;
                snapshot.light.rotation = transformComponent->rotation;
            }
            out.lights.push_back(snapshot.light);
        }

        const bool hasCanvas = coordinator.HasComponent<ECS::Canvas2DComponent>(entity);
        const uint64_t canvasRevision = hasCanvas
            ? HashCanvas(coordinator.GetComponent<ECS::Canvas2DComponent>(entity)) : 0;
        const bool reuseCanvas = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Canvas, hasCanvas, canvasRevision);
        snapshot.hasCanvas = hasCanvas;
        if (hasCanvas && !reuseCanvas) {
            const auto& canvas = coordinator.GetComponent<ECS::Canvas2DComponent>(entity);
            snapshot.canvas.width = canvas.width;
            snapshot.canvas.height = canvas.height;
            snapshot.canvas.stretchToViewport = canvas.stretchToViewport;
            snapshot.canvas.layer = canvas.layer;
        }

        const bool hasSprite = coordinator.HasComponent<ECS::Sprite2DComponent>(entity);
        const uint64_t spriteRevision = hasSprite
            ? HashSprite(coordinator.GetComponent<ECS::Sprite2DComponent>(entity)) : 0;
        const bool reuseSprite = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Sprite, hasSprite, spriteRevision);
        snapshot.hasSprite = hasSprite;
        if (hasSprite && !reuseSprite) {
            const auto& sprite = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
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

        const bool hasButton = coordinator.HasComponent<ECS::ButtonComponent>(entity);
        const uint64_t buttonRevision = hasButton
            ? HashButton(coordinator.GetComponent<ECS::ButtonComponent>(entity)) : 0;
        const bool reuseButton = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Button, hasButton, buttonRevision);
        snapshot.hasButton = hasButton;
        if (hasButton && !reuseButton) {
            const auto& button = coordinator.GetComponent<ECS::ButtonComponent>(entity);
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

        const bool hasText = coordinator.HasComponent<ECS::TextComponent>(entity);
        const uint64_t textRevision = hasText
            ? HashText(coordinator.GetComponent<ECS::TextComponent>(entity)) : 0;
        const bool reuseText = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Text, hasText, textRevision);
        snapshot.hasText = hasText;
        if (hasText && !reuseText) {
            const auto& text = coordinator.GetComponent<ECS::TextComponent>(entity);
            snapshot.text.text = text.text;
            snapshot.text.visible = text.visible;
            snapshot.text.isUI = text.isUI;
            snapshot.text.fontSize = text.fontSize;
            snapshot.text.color = text.color;
            snapshot.text.layer = text.layer;
            snapshot.text.renderMode = ToRenderTextMode(text.renderMode);
            snapshot.text.anchorMin = text.anchorMin;
            snapshot.text.anchorMax = text.anchorMax;
            snapshot.text.pivot = text.pivot;
            snapshot.text.measuredWidth = text.measuredWidth;
            snapshot.text.measuredHeight = text.measuredHeight;
        }

        const bool hasSlice9 = coordinator.HasComponent<ECS::Slice9Component>(entity);
        const uint64_t slice9Revision = hasSlice9
            ? HashSlice9(coordinator.GetComponent<ECS::Slice9Component>(entity)) : 0;
        const bool reuseSlice9 = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Slice9, hasSlice9, slice9Revision);
        snapshot.hasSlice9 = hasSlice9;
        if (hasSlice9 && !reuseSlice9) {
            const auto& slice = coordinator.GetComponent<ECS::Slice9Component>(entity);
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

        const bool hasTilemap = coordinator.HasComponent<ECS::TilemapComponent>(entity);
        const bool reuseTilemap = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Tilemap,
            hasTilemap, hasTilemap ? 1ull : 0ull);
        snapshot.hasTilemap = hasTilemap;
        (void)reuseTilemap;

        const bool hasTerrain = coordinator.HasComponent<ECS::TerrainComponent>(entity) &&
            snapshot.hasTransform;
        const uint64_t terrainRevision = hasTerrain
            ? HashTerrain(coordinator.GetComponent<ECS::TerrainComponent>(entity)) : 0;
        const bool reuseTerrain = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Terrain, hasTerrain, terrainRevision);
        snapshot.hasTerrain = hasTerrain;
        if (hasTerrain) {
            if (!reuseTerrain) {
                snapshot.terrain.entity = entity;
                CopyTerrain(coordinator.GetComponent<ECS::TerrainComponent>(entity), snapshot.terrain);
            }
            if (snapshot.visible && snapshot.terrain.enabled) out.terrains.push_back(snapshot.terrain);
        }

        const bool hasWater = coordinator.HasComponent<ECS::WaterComponent>(entity) &&
            snapshot.hasTransform;
        const uint64_t waterRevision = hasWater
            ? HashWater(coordinator.GetComponent<ECS::WaterComponent>(entity)) : 0;
        const bool reuseWater = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Water, hasWater, waterRevision);
        snapshot.hasWater = hasWater;
        if (hasWater) {
            if (!reuseWater) {
                snapshot.water.entity = entity;
                CopyWater(coordinator.GetComponent<ECS::WaterComponent>(entity), snapshot.water);
            }
            if (snapshot.visible && snapshot.water.enabled) out.waters.push_back(snapshot.water);
        }

        const bool hasSkybox = coordinator.HasComponent<ECS::SkyboxComponent>(entity);
        const uint64_t skyboxRevision = hasSkybox
            ? HashSkybox(coordinator.GetComponent<ECS::SkyboxComponent>(entity)) : 0;
        const bool reuseSkybox = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Skybox, hasSkybox, skyboxRevision);
        snapshot.hasSkybox = hasSkybox;
        if (hasSkybox) {
            if (!reuseSkybox) {
                const auto& skybox = coordinator.GetComponent<ECS::SkyboxComponent>(entity);
                snapshot.skybox.entity = entity;
                snapshot.skybox.enabled = skybox.enabled;
                snapshot.skybox.textureName = skybox.textureName;
                snapshot.skybox.tint = skybox.tint;
                snapshot.skybox.intensity = skybox.intensity;
            }
            out.skyboxes.push_back(snapshot.skybox);
        }

        const bool hasCloud = coordinator.HasComponent<ECS::CloudVolumeComponent>(entity);
        const uint64_t cloudRevision = hasCloud
            ? HashCloud(coordinator.GetComponent<ECS::CloudVolumeComponent>(entity)) : 0;
        const bool reuseCloud = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Cloud, hasCloud, cloudRevision);
        snapshot.hasCloud = hasCloud;
        if (hasCloud) {
            if (!reuseCloud) {
                const auto& cloud = coordinator.GetComponent<ECS::CloudVolumeComponent>(entity);
                snapshot.cloud.entity = entity;
                CopyCloud(cloud, snapshot.cloud);
            }
            out.clouds.push_back(snapshot.cloud);
        }

        const bool hasCollider = coordinator.HasComponent<ECS::ColliderComponent>(entity);
        const uint64_t colliderRevision = hasCollider
            ? HashCollider(coordinator.GetComponent<ECS::ColliderComponent>(entity)) : 0;
        const bool reuseCollider = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::Collider, hasCollider, colliderRevision);
        snapshot.hasCollider = hasCollider;
        if (hasCollider && !reuseCollider) {
            const auto& collider = coordinator.GetComponent<ECS::ColliderComponent>(entity);
            snapshot.collider.type = ToRenderColliderType(collider.type);
            snapshot.collider.offset = collider.offset;
            snapshot.collider.size = collider.size;
            snapshot.collider.isTrigger = collider.isTrigger;
            snapshot.collider.useOBB = collider.useOBB;
        }

        const bool hasRigidBody = coordinator.HasComponent<ECS::RigidBodyComponent>(entity);
        const uint64_t rigidBodyRevision = hasRigidBody
            ? HashRigidBody(coordinator.GetComponent<ECS::RigidBodyComponent>(entity)) : 0;
        const bool reuseRigidBody = PrepareComponentCapture(
            out, snapshot, RenderWorldCaptureComponent::RigidBody, hasRigidBody, rigidBodyRevision);
        snapshot.hasRigidBody = hasRigidBody;
        if (hasRigidBody && !reuseRigidBody) {
            const auto& rigidBody = coordinator.GetComponent<ECS::RigidBodyComponent>(entity);
            snapshot.rigidBody.type = ToRenderRigidBodyType(rigidBody.type);
            snapshot.rigidBody.isTrigger = rigidBody.isTrigger;
            snapshot.rigidBody.shapeType = ToRenderRigidBodyShapeType(rigidBody.shapeType);
            snapshot.rigidBody.size = rigidBody.size;
            snapshot.rigidBody.offset = rigidBody.offset;
            snapshot.rigidBody.generatePerSubmesh = rigidBody.generatePerSubmesh;
        }

    }

    out.entities.resize(entityWriteIndex);
    out.particles = ParticleSystem::GetInstance().GetRenderInstances();
}

void SceneCollector::FinalizeRenderWorld(RenderWorld& out)
{
    // Keep the legacy SceneCollector entry point routed through the same
    // partition implementation used by RenderWorldBuilder.  This prevents
    // the compatibility path from drifting away from the scheduled path.
    RenderWorldBuilder::Finalize(out);
}
