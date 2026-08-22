#pragma once
// SceneCollector.h - scene entity traversal / draw-item collection
// Collects visible draw items from the ECS scene graph, grouped by resource
// path, so the geometry stage can render them in batches. Split out of
// SceneRenderer to keep the renderer focused on drawing.
#include "Platform/Export.h"
#include <vector>
#include <unordered_map>
#include <string>
#include "ECS/Types.h"
#include "SceneTypes.h"

class MIKAN_API SceneCollector {
public:
    // All entities with a visible MeshComponent::Model
    static void CollectModelEntities(ECS::Entity entity, std::vector<ECS::Entity>& out);

    // Renderer key: static meshes share the model path; animated entities get an
    // entity-local key so each AnimatorComponent owns an independent pose.
    static std::string GetModelRendererKey(ECS::Entity entity);

    // Visible mesh entities grouped by modelPath / animation instance (batch draw items)
    static void CollectModelEntitiesByPath(ECS::Entity entity, std::unordered_map<std::string, ModelInstanceGroup>& out);

    // Visible vox entities grouped by voxPath
    static void CollectVoxModelEntitiesByPath(ECS::Entity entity, std::unordered_map<std::string, VoxInstanceGroup>& out);

    // Entities with a LightComponent
    static void CollectLightEntities(ECS::Entity entity, std::vector<ECS::Entity>& out);

    // Entities with a CameraComponent
    static void CollectCameraEntities(ECS::Entity entity, std::vector<ECS::Entity>& out);
};
