#include "Core/EngineDiagnostics.h"

#include "Core/GameplayRuntime.h"
#include "Core/ProjectManager.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "SceneSerializer.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <vector>

namespace Core {
namespace {

const char* FieldTypeName(ECS::FieldType type)
{
    switch (type) {
        case ECS::FieldType::Bool:      return "Bool";
        case ECS::FieldType::Int:       return "Int";
        case ECS::FieldType::Float:     return "Float";
        case ECS::FieldType::Vec2:      return "Vec2";
        case ECS::FieldType::Vec3:      return "Vec3";
        case ECS::FieldType::Vec4:      return "Vec4";
        case ECS::FieldType::Color3:    return "Color3";
        case ECS::FieldType::Color4:    return "Color4";
        case ECS::FieldType::QuatEuler: return "QuatEuler";
        case ECS::FieldType::String:    return "String";
        case ECS::FieldType::Path:      return "Path";
        case ECS::FieldType::Enum:      return "Enum";
        case ECS::FieldType::Hidden:    return "Hidden";
    }
    return "Unknown";
}

} // namespace

void DumpSchema(const std::string& path)
{
    FILE* file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, path.c_str(), "w") != 0 || !file) {
#else
    file = std::fopen(path.c_str(), "w");
    if (!file) {
#endif
        std::fprintf(stderr, "[Schema] ERROR: cannot open output: %s\n", path.c_str());
        return;
    }

    auto& registry = ECS::ComponentRegistry::GetInstance();
    const auto& all = registry.GetAll();
    std::fprintf(file, "{\n  \"components\": [\n");
    for (size_t componentIndex = 0; componentIndex < all.size(); ++componentIndex) {
        const auto& meta = all[componentIndex];
        std::fprintf(file,
            "    {\"serializeKey\": %s, \"displayName\": \"%s\", \"category\": \"%s\", \"fields\": [",
            meta.serializeKey ? (std::string("\"") + meta.serializeKey + "\"").c_str() : "null",
            meta.displayName ? meta.displayName : "",
            meta.category ? meta.category : "");
        for (size_t fieldIndex = 0; fieldIndex < meta.fieldCount; ++fieldIndex) {
            const auto& field = meta.fields[fieldIndex];
            std::fprintf(file, "%s{\"name\": \"%s\", \"type\": \"%s\"}",
                fieldIndex ? ", " : "", field.name ? field.name : "", FieldTypeName(field.type));
        }
        // 脚本组件的 params 是对象原文（SceneSerializer 手写序列化，不在字段表里），
        // 补入 schema 供 validate_scene 字段白名单校验通过。
        if (meta.serializeKey && std::strcmp(meta.serializeKey, "script") == 0) {
            std::fprintf(file, "%s{\"name\": \"params\", \"type\": \"Object\"}",
                meta.fieldCount ? ", " : "");
        }
        std::fprintf(file, "]}");
        if (componentIndex + 1 < all.size()) std::fprintf(file, ",");
        std::fprintf(file, "\n");
    }
    std::fprintf(file, "  ]\n}\n");
    std::fclose(file);
    std::fprintf(stderr, "[Schema] Dumped %zu component metas -> %s\n", all.size(), path.c_str());
}

int RunPrefabSelftest()
{
    auto& scene = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    const ECS::Entity rootEntity = scene.FindByName("CesiumMan");
    if (rootEntity == ECS::INVALID_ENTITY) {
        std::printf("[PrefabSelftest] FAIL: entity 'CesiumMan' not found in scene\n");
        return 1;
    }

    auto collectTree = [&](ECS::Entity root) -> std::vector<ECS::Entity> {
        std::vector<ECS::Entity> result;
        std::vector<ECS::Entity> stack{root};
        while (!stack.empty()) {
            const ECS::Entity entity = stack.back();
            stack.pop_back();
            result.push_back(entity);
            for (const ECS::Entity child : scene.GetChildren(entity)) stack.push_back(child);
        }
        return result;
    };

    const auto originalTree = collectTree(rootEntity);
    const std::string prefabDirectory = ProjectManager::GetInstance().ResolveAssetPath("prefabs/");
    std::filesystem::create_directories(prefabDirectory);
    const std::string prefabPath = prefabDirectory + "CesiumMan.prefab.json";

    ECS::SceneSerializer serializer;
    if (!serializer.SavePrefab(rootEntity, prefabPath)) {
        std::printf("[PrefabSelftest] FAIL: SavePrefab\n");
        return 1;
    }
    const ECS::Entity instance = serializer.InstantiatePrefab(prefabPath);
    if (instance == ECS::INVALID_ENTITY) {
        std::printf("[PrefabSelftest] FAIL: InstantiatePrefab\n");
        return 1;
    }
    const auto newTree = collectTree(instance);

    bool ok = (newTree.size() == originalTree.size());
    bool transformMatch = false;
    if (coordinator.HasComponent<ECS::TransformComponent>(instance) &&
        coordinator.HasComponent<ECS::TransformComponent>(rootEntity)) {
        auto& instanceTransform = coordinator.GetComponent<ECS::TransformComponent>(instance);
        auto& rootTransform = coordinator.GetComponent<ECS::TransformComponent>(rootEntity);
        transformMatch = (instanceTransform.position == rootTransform.position) &&
                         (instanceTransform.scale == rootTransform.scale);
        ok = ok && transformMatch;
    }

    const bool hasMesh = coordinator.HasComponent<ECS::MeshComponent>(instance);
    const bool hasRender = coordinator.HasComponent<ECS::RenderComponent>(instance);
    const bool hasMaterial = coordinator.HasComponent<ECS::MaterialComponent>(instance);
    const bool hasScript = coordinator.HasComponent<ECS::ScriptComponent>(instance);
    ok = ok && hasMesh && hasRender && hasMaterial && hasScript;
    const bool scriptInstance = hasScript &&
        (coordinator.GetComponent<ECS::ScriptComponent>(instance).runtime != nullptr);
    ok = ok && scriptInstance;

    std::printf("[PrefabSelftest] orig_tree=%zu new_tree=%zu transform=%s mesh=%d render=%d material=%d script=%d script_inst=%d -> %s\n",
        originalTree.size(), newTree.size(), transformMatch ? "match" : "DIFF",
        hasMesh ? 1 : 0, hasRender ? 1 : 0, hasMaterial ? 1 : 0, hasScript ? 1 : 0,
        scriptInstance ? 1 : 0, ok ? "PASS" : "FAIL");

    ECS::Entity parent = scene.CreateCube("PrefabParent");
    ECS::Entity child = scene.CreateCube("PrefabChild");
    scene.SetPosition(parent, glm::vec3(1.0f, 2.0f, 3.0f));
    scene.SetParent(child, parent);

    const std::string treePath = prefabDirectory + "TreeTest.prefab.json";
    const bool treeSaved = serializer.SavePrefab(parent, treePath);
    const ECS::Entity treeInstance = treeSaved
        ? serializer.InstantiatePrefab(treePath)
        : ECS::INVALID_ENTITY;
    bool treeSizeOk = false;
    bool treeChildOk = false;
    bool treePositionOk = false;
    if (treeInstance != ECS::INVALID_ENTITY) {
        const auto newTree2 = collectTree(treeInstance);
        treeSizeOk = (newTree2.size() == 2);
        treeChildOk = (scene.GetChildren(treeInstance).size() == 1);
        treePositionOk = (scene.GetPosition(treeInstance) == glm::vec3(1.0f, 2.0f, 3.0f));
    }

    std::function<void(ECS::Entity)> deleteTree = [&](ECS::Entity entity) {
        for (const ECS::Entity descendant : scene.GetChildren(entity)) deleteTree(descendant);
        scene.DestroyEntity(entity);
    };
    deleteTree(parent);
    if (treeInstance != ECS::INVALID_ENTITY) deleteTree(treeInstance);

    const bool treeOk = treeSaved && treeInstance != ECS::INVALID_ENTITY &&
                        treeSizeOk && treeChildOk && treePositionOk;
    std::printf("[PrefabSelftest] tree: save=%d instantiate=%d size=%d child=%d pos=%d -> %s\n",
        treeSaved ? 1 : 0, treeInstance != ECS::INVALID_ENTITY ? 1 : 0,
        treeSizeOk ? 1 : 0, treeChildOk ? 1 : 0, treePositionOk ? 1 : 0,
        treeOk ? "PASS" : "FAIL");

    return (ok && treeOk) ? 0 : 1;
}

bool DumpSceneState(const std::string& path, int frames, float fps)
{
    return Core::GameplayRuntime::DumpState(path, frames, "render-vulkan", fps);
}

} // namespace Core
