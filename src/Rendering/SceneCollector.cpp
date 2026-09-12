// SceneCollector.cpp - scene entity traversal / draw-item collection
#include "Rendering/SceneCollector.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include <utility>

void SceneCollector::CollectModelEntities(ECS::Entity entity, std::vector<ECS::Entity>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::MeshComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {
        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible) {
            auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            if (mesh.type == ECS::MeshType::Model)
                out.push_back(entity);
        }
    }
    for (const auto& child : sceneECS.GetChildren(entity))
        CollectModelEntities(child, out);
}

std::string SceneCollector::GetModelRendererKey(ECS::Entity entity)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::MeshComponent>(entity)) return {};

    const auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
    if (mesh.modelPath.empty()) return {};

    // ModelRenderer stores animation time/bones internally. An entity with an
    // AnimatorComponent therefore cannot share the path-only renderer with a
    // different animated entity, even when both load the same asset.
    if (!coordinator.HasComponent<ECS::AnimatorComponent>(entity) &&
        !coordinator.HasComponent<ECS::VmdPlayerComponent>(entity)) {
        return mesh.modelPath;
    }
    return mesh.modelPath + "#entity:" + std::to_string(entity);
}

void SceneCollector::CollectModelEntitiesByPath(ECS::Entity entity, std::unordered_map<std::string, ModelInstanceGroup>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::MeshComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {
        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible) {
            auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            if ((mesh.type == ECS::MeshType::Model || mesh.type == ECS::MeshType::Plane) && !mesh.modelPath.empty()) {
                const std::string rendererKey = GetModelRendererKey(entity);
                auto it = out.find(rendererKey);
                if (it == out.end()) {
                    ModelInstanceGroup group;
                    group.modelPath = mesh.modelPath;
                    group.entities.push_back(entity);
                    out[rendererKey] = group;
                } else {
                    it->second.entities.push_back(entity);
                }
            }
        }
    }
    for (const auto& child : sceneECS.GetChildren(entity))
        CollectModelEntitiesByPath(child, out);
}

void SceneCollector::CollectModelEntitiesByPath(
    const std::vector<ECS::Entity>& entities,
    std::unordered_map<std::string, ModelInstanceGroup>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    for (const ECS::Entity entity : entities) {
        if (!coordinator.HasComponent<ECS::TransformComponent>(entity) ||
            !coordinator.HasComponent<ECS::MeshComponent>(entity) ||
            !coordinator.HasComponent<ECS::RenderComponent>(entity)) {
            continue;
        }

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (!render.visible) continue;
        auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
        if ((mesh.type != ECS::MeshType::Model && mesh.type != ECS::MeshType::Plane) ||
            mesh.modelPath.empty()) {
            continue;
        }

        const std::string rendererKey = GetModelRendererKey(entity);
        auto it = out.find(rendererKey);
        if (it == out.end()) {
            ModelInstanceGroup group;
            group.modelPath = mesh.modelPath;
            group.entities.push_back(entity);
            out.emplace(rendererKey, std::move(group));
        } else {
            it->second.entities.push_back(entity);
        }
    }
}

void SceneCollector::CollectVoxModelEntitiesByPath(ECS::Entity entity, std::unordered_map<std::string, VoxInstanceGroup>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
        auto& voxComp = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
        if (!voxComp.voxPath.empty()) {
            auto it = out.find(voxComp.voxPath);
            if (it == out.end()) {
                VoxInstanceGroup group;
                group.voxPath = voxComp.voxPath;
                group.entities.push_back(entity);
                out[voxComp.voxPath] = group;
            } else {
                it->second.entities.push_back(entity);
            }
        }
    }
    for (const auto& child : sceneECS.GetChildren(entity))
        CollectVoxModelEntitiesByPath(child, out);
}

void SceneCollector::CollectVoxModelEntitiesByPath(
    const std::vector<ECS::Entity>& entities,
    std::unordered_map<std::string, VoxInstanceGroup>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    for (const ECS::Entity entity : entities) {
        if (!coordinator.HasComponent<ECS::VoxModelComponent>(entity)) continue;
        auto& voxComp = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
        if (voxComp.voxPath.empty()) continue;

        auto it = out.find(voxComp.voxPath);
        if (it == out.end()) {
            VoxInstanceGroup group;
            group.voxPath = voxComp.voxPath;
            group.entities.push_back(entity);
            out.emplace(voxComp.voxPath, std::move(group));
        } else {
            it->second.entities.push_back(entity);
        }
    }
}

void SceneCollector::CollectLightEntities(ECS::Entity entity, std::vector<ECS::Entity>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::LightComponent>(entity))
        out.push_back(entity);
    for (const auto& child : sceneECS.GetChildren(entity))
        CollectLightEntities(child, out);
}

void SceneCollector::CollectLightEntities(
    const std::vector<ECS::Entity>& entities,
    std::vector<ECS::Entity>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    for (const ECS::Entity entity : entities) {
        if (coordinator.HasComponent<ECS::LightComponent>(entity)) {
            out.push_back(entity);
        }
    }
}

void SceneCollector::CollectCameraEntities(ECS::Entity entity, std::vector<ECS::Entity>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::CameraComponent>(entity))
        out.push_back(entity);
    for (const auto& child : sceneECS.GetChildren(entity))
        CollectCameraEntities(child, out);
}

void SceneCollector::CollectCameraEntities(
    const std::vector<ECS::Entity>& entities,
    std::vector<ECS::Entity>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    for (const ECS::Entity entity : entities) {
        if (coordinator.HasComponent<ECS::CameraComponent>(entity)) {
            out.push_back(entity);
        }
    }
}
