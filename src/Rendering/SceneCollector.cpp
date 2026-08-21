// SceneCollector.cpp - scene entity traversal / draw-item collection
#include "Rendering/SceneCollector.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"

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
                auto it = out.find(mesh.modelPath);
                if (it == out.end()) {
                    ModelInstanceGroup group;
                    group.modelPath = mesh.modelPath;
                    group.entities.push_back(entity);
                    out[mesh.modelPath] = group;
                } else {
                    it->second.entities.push_back(entity);
                }
            }
        }
    }
    for (const auto& child : sceneECS.GetChildren(entity))
        CollectModelEntitiesByPath(child, out);
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

void SceneCollector::CollectLightEntities(ECS::Entity entity, std::vector<ECS::Entity>& out)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::LightComponent>(entity))
        out.push_back(entity);
    for (const auto& child : sceneECS.GetChildren(entity))
        CollectLightEntities(child, out);
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