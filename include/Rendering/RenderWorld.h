#pragma once

// RenderWorld.h - immutable CPU scene snapshot consumed by rendering passes.
//
// The snapshot is intentionally free of Vulkan handles, ECS component types,
// callbacks, and runtime pointers.  SceneCollector is the extraction boundary;
// render code can retain a const RenderWorld for all passes recorded for one
// frame without touching the live ECS registry.

#include "Platform/Export.h"
#include "ECS/Types.h"
#include "Rendering/ParticleSystem.h"
#include "Rendering/SceneTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

enum class RenderMeshType : uint8_t {
    None,
    Cube,
    Sphere,
    Plane,
    Model,
    Cylinder,
    Cone,
    Capsule,
    Torus,
    Pyramid
};

enum class RenderLightType : uint8_t {
    Directional,
    Point,
    Spot
};

enum class RenderSpriteType : uint8_t {
    Rect,
    Sprite,
    Button
};

enum class RenderTextMode : uint8_t {
    Bitmap,
    Sdf,
    Msdf
};

enum class RenderColliderType : uint8_t {
    Box,
    Sphere,
    Capsule
};

enum class RenderRigidBodyType : uint8_t {
    Static,
    Dynamic,
    Kinematic
};

enum class RenderRigidBodyShapeType : uint8_t {
    Box,
    Sphere,
    Capsule,
    OBB,
    Mesh
};

struct MIKAN_API RenderTransformData {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
    glm::mat4 worldMatrix = glm::mat4(1.0f);
    glm::vec3 eulerAngles = glm::vec3(0.0f);
};

struct MIKAN_API RenderMeshData {
    RenderMeshType type = RenderMeshType::None;
    std::string modelPath;
};

struct MIKAN_API RenderFlagsData {
    bool visible = true;
    bool castShadow = true;
    bool receiveShadow = true;
    bool showAABB = false;
    bool showOBB = false;
    bool doubleSided = false;
    bool wireframe = false;
};

struct MIKAN_API RenderVoxelData {
    std::string voxPath;
    bool loaded = false;
    bool isStatic = true;
};

struct MIKAN_API RenderAnimatorData {
    int clipIndex = 0;
    float speed = 1.0f;
    bool loop = true;
    bool playing = true;
    float time = 0.0f;
};

enum class RenderVmdTarget : uint8_t {
    Auto,
    Model,
    Camera
};

struct MIKAN_API RenderVmdData {
    RenderVmdTarget target = RenderVmdTarget::Auto;
    bool enabled = true;
    bool hasMotion = false;
};

struct MIKAN_API RenderCameraData {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool isMainCamera = false;
    bool isOrthographic = false;
    float orthographicSize = 5.0f;
    bool enableFrustumCulling = false;
    bool showFrustumWireframe = true;
    bool useSubMeshCulling = true;
    bool showBVHWireframe = false;
    bool showCollisionWireframe = false;
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    std::string postProcessChain;

    glm::mat4 GetProjectionMatrix(float aspectRatio) const {
        if (isOrthographic) {
            return glm::ortho(-orthographicSize * aspectRatio,
                              orthographicSize * aspectRatio,
                              -orthographicSize, orthographicSize,
                              nearPlane, farPlane);
        }
        return glm::perspective(glm::radians(fov), aspectRatio,
                                nearPlane, farPlane);
    }

    glm::mat4 GetViewMatrix() const {
        const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::lookAt(position, position + forward, up);
    }
};

struct MIKAN_API RenderLightData {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    RenderLightType type = RenderLightType::Directional;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float range = 10.0f;
    float spotAngle = 45.0f;
    bool castShadow = true;
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
};

struct MIKAN_API RenderCanvasData {
    float width = 1920.0f;
    float height = 1080.0f;
    bool stretchToViewport = true;
    float layer = 0.0f;
};

struct MIKAN_API RenderSpriteData {
    RenderSpriteType type = RenderSpriteType::Rect;
    bool visible = true;
    bool isUI = false;
    float width = 64.0f;
    float height = 64.0f;
    glm::vec2 uv0 = glm::vec2(0.0f);
    glm::vec2 uv1 = glm::vec2(1.0f);
    glm::vec4 color = glm::vec4(1.0f);
    std::string texture;
    int layer = 0;
    glm::vec2 anchorMin = glm::vec2(0.0f);
    glm::vec2 anchorMax = glm::vec2(0.0f);
    bool hovered = false;
    std::string label;
    float labelFontSize = 0.0f;
    glm::vec4 labelColor = glm::vec4(1.0f);
};

struct MIKAN_API RenderButtonData {
    bool visible = true;
    bool isUI = false;
    float width = 160.0f;
    float height = 48.0f;
    glm::vec4 fillColor = glm::vec4(0.15f, 0.15f, 0.18f, 1.0f);
    glm::vec4 hoverColor = glm::vec4(0.25f, 0.30f, 0.45f, 1.0f);
    glm::vec4 textColor = glm::vec4(1.0f);
    std::string text;
    float fontSize = 0.0f;
    int layer = 0;
    bool hovered = false;
};

struct MIKAN_API RenderTextData {
    std::string text = "Text";
    bool visible = true;
    bool isUI = false;
    float fontSize = 24.0f;
    glm::vec4 color = glm::vec4(1.0f);
    int layer = 0;
    RenderTextMode renderMode = RenderTextMode::Msdf;
    float measuredWidth = 0.0f;
    float measuredHeight = 0.0f;
};

struct MIKAN_API RenderSlice9Data {
    std::string texture;
    bool visible = true;
    bool isUI = true;
    float width = 128.0f;
    float height = 128.0f;
    glm::vec4 border = glm::vec4(8.0f);
    glm::vec2 uv0 = glm::vec2(0.0f);
    glm::vec2 uv1 = glm::vec2(1.0f);
    glm::vec4 color = glm::vec4(1.0f);
    int layer = 0;
    float texWidth = -1.0f;
    float texHeight = -1.0f;
};

struct MIKAN_API RenderTerrainData {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    bool enabled = true;
    std::string heightmapPath;
    glm::vec2 worldSize = glm::vec2(256.0f);
    float heightScale = 64.0f;
    float heightOffset = 0.0f;
    int chunkCount = 8;
    int patchResolution = 33;
    float viewDistance = 2000.0f;
    float lod0Distance = 200.0f;
    float lod1Distance = 600.0f;
    int maxLod = 2;
    bool wireframe = false;
    float materialTiling = 8.0f;
    float blendSharpness = 1.0f;
    std::string layer0Path;
    std::string layer1Path;
    std::string layer2Path;
    std::string layer3Path;
    std::string controlMapPath;
};

struct MIKAN_API RenderWaterData {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    bool enabled = true;
    glm::vec2 size = glm::vec2(32.0f);
    float surfaceOffset = 0.0f;
    float depth = 20.0f;
    float buoyancy = 1.15f;
    float drag = 2.0f;
    glm::vec3 color = glm::vec3(0.035f, 0.22f, 0.32f);
    float roughness = 0.12f;
    bool affectPlayersOnly = true;
};

struct MIKAN_API RenderSkyboxData {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    bool enabled = true;
    std::string textureName = "skybox";
    glm::vec3 tint = glm::vec3(1.0f);
    float intensity = 1.0f;
};

struct MIKAN_API RenderCloudData {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    bool enabled = true;
    float coverage = 0.45f;
    float density = 0.55f;
    float baseAltitudeKm = 7.5f;
    float thicknessKm = 12.5f;
    float noiseScale = 0.01f;
    float detailErosion = 0.24f;
    float detailScale = 4.0f;
    float lightAbsorption = 1.0f;
    float multipleScattering = 0.22f;
    float multipleScatteringBuild = 0.15f;
    float multipleScatteringBoundary = 0.65f;
    float multipleScatteringCompress = 0.35f;
    glm::vec3 noiseOffsetKm = glm::vec3(37.0f, 13.0f, -61.0f);
    float windSpeedKmPerSecond = 0.01f;
    glm::vec2 windDirectionXZ = glm::vec2(1.0f, 0.0f);
    bool highCloudEnabled = true;
    float highCloudCoverage = 0.28f;
    float highCloudDensity = 0.35f;
    float highCloudAltitudeKm = 18.0f;
    float highCloudThicknessKm = 1.0f;
    float highCloudScale = 0.0045f;
    float highCloudDetail = 0.45f;
    float highCloudBrightness = 0.32f;
    float highCloudWindSpeedKmPerSecond = 0.006f;
    glm::vec2 highCloudWindDirectionXZ = glm::vec2(0.35f, 1.0f);
};

struct MIKAN_API RenderColliderData {
    RenderColliderType type = RenderColliderType::Box;
    glm::vec3 offset = glm::vec3(0.0f);
    glm::vec3 size = glm::vec3(1.0f);
    bool isTrigger = false;
    bool useOBB = false;
};

struct MIKAN_API RenderRigidBodyData {
    RenderRigidBodyType type = RenderRigidBodyType::Dynamic;
    bool isTrigger = false;
    RenderRigidBodyShapeType shapeType = RenderRigidBodyShapeType::Box;
    glm::vec3 size = glm::vec3(1.0f);
    glm::vec3 offset = glm::vec3(0.0f);
    bool generatePerSubmesh = false;
};

struct MIKAN_API RenderWorldEntity {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    ECS::Entity parent = ECS::INVALID_ENTITY;
    std::vector<ECS::Entity> children;

    bool visible = true;
    bool hasTransform = false;
    RenderTransformData transform;
    bool hasMesh = false;
    RenderMeshData mesh;
    bool hasMaterial = false;
    RenderMaterialData material;
    bool hasRenderFlags = false;
    RenderFlagsData render;
    bool hasVoxel = false;
    RenderVoxelData voxel;
    bool hasAnimator = false;
    RenderAnimatorData animator;
    bool hasVmdPlayer = false;
    RenderVmdData vmd;
    bool hasCamera = false;
    RenderCameraData camera;
    bool hasCamera2D = false;
    bool camera2DEnabled = false;
    bool hasLight = false;
    RenderLightData light;
    bool hasCanvas = false;
    RenderCanvasData canvas;
    bool hasSprite = false;
    RenderSpriteData sprite;
    bool hasButton = false;
    RenderButtonData button;
    bool hasText = false;
    RenderTextData text;
    bool hasSlice9 = false;
    RenderSlice9Data slice9;
    bool hasTilemap = false;
    bool hasTerrain = false;
    RenderTerrainData terrain;
    bool hasWater = false;
    RenderWaterData water;
    bool hasSkybox = false;
    RenderSkyboxData skybox;
    bool hasCloud = false;
    RenderCloudData cloud;
    bool hasCollider = false;
    RenderColliderData collider;
    bool hasRigidBody = false;
    RenderRigidBodyData rigidBody;

    // Reset only the frame-ownership fields while retaining string/vector
    // capacities.  RenderWorldBuilder uses this when reusing the write
    // buffer, so a stable scene does not allocate for every entity again.
    void ResetForBuild() {
        entity = ECS::INVALID_ENTITY;
        parent = ECS::INVALID_ENTITY;
        children.clear();
        visible = true;

        hasTransform = false;
        hasMesh = false;
        mesh.modelPath.clear();
        hasMaterial = false;
        material.albedoPath.clear();
        material.normalPath.clear();
        material.roughnessPath.clear();
        material.metallicPath.clear();
        material.aoPath.clear();
        material.emissivePath.clear();
        hasRenderFlags = false;
        hasVoxel = false;
        voxel.voxPath.clear();
        hasAnimator = false;
        hasVmdPlayer = false;
        vmd = RenderVmdData{};
        hasCamera = false;
        camera.postProcessChain.clear();
        hasCamera2D = false;
        camera2DEnabled = false;
        hasLight = false;
        hasCanvas = false;
        hasSprite = false;
        sprite.texture.clear();
        sprite.label.clear();
        hasButton = false;
        button.text.clear();
        hasText = false;
        text.text.clear();
        hasSlice9 = false;
        slice9.texture.clear();
        hasTilemap = false;
        hasTerrain = false;
        terrain.heightmapPath.clear();
        terrain.layer0Path.clear();
        terrain.layer1Path.clear();
        terrain.layer2Path.clear();
        terrain.layer3Path.clear();
        terrain.controlMapPath.clear();
        hasWater = false;
        hasSkybox = false;
        skybox.textureName.clear();
        hasCloud = false;
        hasCollider = false;
        hasRigidBody = false;
    }
};

struct MIKAN_API RenderModelGroup {
    std::string rendererKey;
    std::string modelPath;
    std::vector<ECS::Entity> entities;
};

struct MIKAN_API RenderVoxGroup {
    std::string voxPath;
    std::vector<ECS::Entity> entities;
};

struct MIKAN_API RenderWorld {
    uint64_t frameNumber = 0;
    uint32_t entitySetVersion = 0;
    ECS::Entity selectedEntity = ECS::INVALID_ENTITY;

    std::vector<ECS::Entity> rootEntities;
    std::vector<ECS::Entity> hierarchyEntities;
    std::vector<RenderWorldEntity> entities;
    std::vector<RenderModelGroup> modelGroups;
    std::vector<RenderVoxGroup> voxGroups;
    std::vector<RenderCameraData> cameras;
    std::vector<RenderLightData> lights;
    std::vector<RenderTerrainData> terrains;
    std::vector<RenderWaterData> waters;
    std::vector<RenderSkyboxData> skyboxes;
    std::vector<RenderCloudData> clouds;
    std::vector<ParticleInstance> particles;

    // Prepare this instance as a reusable write buffer.  Existing entity
    // records are reset in place where possible; top-level containers retain
    // their capacity between frames.
    void BeginBuild(std::size_t expectedEntityCount) {
        frameNumber = 0;
        entitySetVersion = 0;
        selectedEntity = ECS::INVALID_ENTITY;
        rootEntities.clear();
        hierarchyEntities.clear();
        modelGroups.clear();
        voxGroups.clear();
        cameras.clear();
        lights.clear();
        terrains.clear();
        waters.clear();
        skyboxes.clear();
        clouds.clear();
        particles.clear();
        entities.resize(expectedEntityCount);
        for (auto& entity : entities) {
            entity.ResetForBuild();
        }
        indexByEntity.clear();
    }

    void Clear() {
        frameNumber = 0;
        entitySetVersion = 0;
        selectedEntity = ECS::INVALID_ENTITY;
        rootEntities.clear();
        hierarchyEntities.clear();
        entities.clear();
        modelGroups.clear();
        voxGroups.clear();
        cameras.clear();
        lights.clear();
        terrains.clear();
        waters.clear();
        skyboxes.clear();
        clouds.clear();
        particles.clear();
        indexByEntity.clear();
    }

    const RenderWorldEntity* Find(ECS::Entity entity) const {
        const auto it = indexByEntity.find(entity);
        if (it == indexByEntity.end() || it->second >= entities.size()) return nullptr;
        return &entities[it->second];
    }

    void RebuildIndex() {
        indexByEntity.clear();
        indexByEntity.reserve(entities.size());
        for (size_t i = 0; i < entities.size(); ++i) {
            indexByEntity[entities[i].entity] = i;
        }
    }

    // Structural consistency check used by opt-in runtime validation and the
    // synthetic large-scene test.  It does not inspect live ECS state.
    bool Validate(std::string* error = nullptr) const;

private:
    std::unordered_map<ECS::Entity, size_t> indexByEntity;
};
