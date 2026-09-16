#include "Core/Log.h"
#include "Core/EngineDiagnostics.h"

#include "Core/GameplayRuntime.h"
#include "Core/ProjectManager.h"
#include "Core/Utf8Path.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "SceneSerializer.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
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
        LOGE("[Schema] ERROR: cannot open output: %s", path.c_str());
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
    LOGI("[Schema] Dumped %zu component metas -> %s", all.size(), path.c_str());
}

int RunPrefabSelftest()
{
    auto& scene = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();

    // 角色实体的名字随场景原型演进而变（CesiumWalk 的 FindPlayerEntity 也维护同一组候选）。
    // 这里必须用同一组候选名逐个尝试：早先硬编码 "CesiumMan" 后，现存的
    // third-person-navigation 场景把根实体叫 "AnimationPlayer"，自测于是恒 FAIL。
    const char* rootCandidates[] = {"CesiumMan", "AnimationPlayer", "FoxPlayer"};
    ECS::Entity rootEntity = ECS::INVALID_ENTITY;
    const char* rootEntityName = nullptr;
    for (const char* candidate : rootCandidates) {
        const ECS::Entity found = scene.FindByName(candidate);
        if (found != ECS::INVALID_ENTITY) {
            rootEntity = found;
            rootEntityName = candidate;
            break;
        }
    }
    if (rootEntity == ECS::INVALID_ENTITY) {
        LOGE("[PrefabSelftest] FAIL: no player entity found (tried CesiumMan/AnimationPlayer/FoxPlayer)");
        return 1;
    }
    LOGI("[PrefabSelftest] root entity resolved: '%s' (id=%u)", rootEntityName, rootEntity);

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

    // 探针文件写在 <engineRoot>out/prefab-selftest/ 下，不落在用户项目里。
    // 早先直接用 ResolveAssetPath("prefabs/") 当探针目录，每跑一次自测就往用户
    // 项目写 CesiumMan.prefab.json / TreeTest.prefab.json，把 git status 弄脏。
    // 落点是否"在项目内且可写"由下面的 landing 环节单独验证，不靠污染项目来证明。
    const std::string scratchRaw = ProjectManager::GetInstance().GetEngineRoot() +
                                   "out/prefab-selftest/";
    const std::string prefabDirectory = Utf8Path(scratchRaw).generic_string();
    std::error_code scratchEc;
    std::filesystem::create_directories(Utf8Path(prefabDirectory), scratchEc);
    if (!std::filesystem::is_directory(Utf8Path(prefabDirectory), scratchEc)) {
        LOGE("[PrefabSelftest] FAIL: cannot create scratch dir '%s'", prefabDirectory.c_str());
        return 1;
    }
    const std::string prefabPath = prefabDirectory + "CesiumMan.prefab.json";

    ECS::SceneSerializer serializer;
    if (!serializer.SavePrefab(rootEntity, prefabPath)) {
        LOGE("[PrefabSelftest] FAIL: SavePrefab");
        return 1;
    }
    const ECS::Entity instance = serializer.InstantiatePrefab(prefabPath);
    if (instance == ECS::INVALID_ENTITY) {
        LOGE("[PrefabSelftest] FAIL: InstantiatePrefab");
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
    const bool hasScriptOnOriginal =
        coordinator.HasComponent<ECS::ScriptComponent>(rootEntity);
    ok = ok && hasMesh && hasRender && hasMaterial;
    // 往返必须逐组件对账：原实体有什么，实例就得有什么。
    // 旧断言无条件要求 hasScript，等于假定根实体一定带脚本——而场景原型里的
    // 角色实体(AnimationPlayer)并不带脚本，这条会误报 FAIL。
    ok = ok && (hasScript == hasScriptOnOriginal);
    const bool scriptInstance = hasScript &&
        (coordinator.GetComponent<ECS::ScriptComponent>(instance).runtime != nullptr);
    // 只有原本就有脚本时才要求实例化出 runtime（无脚本实体无从谈起）。
    ok = ok && (!hasScriptOnOriginal || scriptInstance);

    LOGI("[PrefabSelftest] orig_tree=%zu new_tree=%zu transform=%s mesh=%d render=%d material=%d script=%d(orig=%d) script_inst=%d -> %s",
        originalTree.size(), newTree.size(), transformMatch ? "match" : "DIFF",
        hasMesh ? 1 : 0, hasRender ? 1 : 0, hasMaterial ? 1 : 0,
        hasScript ? 1 : 0, hasScriptOnOriginal ? 1 : 0,
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
    LOGI("[PrefabSelftest] tree: save=%d instantiate=%d size=%d child=%d pos=%d -> %s",
        treeSaved ? 1 : 0, treeInstance != ECS::INVALID_ENTITY ? 1 : 0,
        treeSizeOk ? 1 : 0, treeChildOk ? 1 : 0, treePositionOk ? 1 : 0,
        treeOk ? "PASS" : "FAIL");

    // 落点自测：编辑器「保存为预制体」写的是 ResolveAssetPath("prefabs/<name>.prefab.json")。
    // 这里验证两件事——① 该路径确实落在当前项目目录内且父目录可创建；② 不带脚本的实体
    // 也能完整往返（旧自测只覆盖带 ScriptComponent 的角色实体，漏掉"无脚本"这条常见路径）。
    // 探针文件用完即删，目录若由本次新建且仍为空也一并收走，保证不弄脏用户项目。
    const std::string projectPrefabDir =
        ProjectManager::GetInstance().ResolveAssetPath("prefabs/");
    bool landingDirOk = false;
    bool landingInsideProject = false;
    bool plainRoundTripOk = false;
    bool landingWriteOk = false;
    if (!projectPrefabDir.empty()) {
        std::error_code ec;
        const bool dirExistedBefore = std::filesystem::is_directory(Utf8Path(projectPrefabDir), ec);
        std::filesystem::create_directories(Utf8Path(projectPrefabDir), ec);
        landingDirOk = std::filesystem::is_directory(Utf8Path(projectPrefabDir), ec);

        const std::string projectRoot = ProjectManager::GetInstance().GetProjectRoot();
        if (!projectRoot.empty()) {
            const auto dirCanonical =
                std::filesystem::weakly_canonical(Utf8Path(projectPrefabDir), ec);
            const auto rootCanonical =
                std::filesystem::weakly_canonical(Utf8Path(projectRoot), ec);
            if (!ec) {
                const std::string d = dirCanonical.generic_string();
                const std::string r = rootCanonical.generic_string();
                landingInsideProject = (d == r) || (d.rfind(r + "/", 0) == 0);
            }
        }

        if (landingDirOk) {
            const std::string landingPath = projectPrefabDir + "PrefabLandingTest.prefab.json";
            ECS::Entity plain = scene.CreateEmpty("PrefabLandingRoot");
            ECS::Entity plainChild = scene.CreateEmpty("PrefabLandingChild");
            scene.SetPosition(plain, glm::vec3(7.0f, -2.5f, 4.0f));
            scene.SetParent(plainChild, plain);
            landingWriteOk = serializer.SavePrefab(plain, landingPath);
            const ECS::Entity plainInstance = landingWriteOk
                ? serializer.InstantiatePrefab(landingPath)
                : ECS::INVALID_ENTITY;
            if (plainInstance != ECS::INVALID_ENTITY) {
                plainRoundTripOk =
                    (collectTree(plainInstance).size() == 2) &&
                    (scene.GetChildren(plainInstance).size() == 1) &&
                    (scene.GetPosition(plainInstance) == glm::vec3(7.0f, -2.5f, 4.0f));
                deleteTree(plainInstance);
            }
            deleteTree(plain);
            std::filesystem::remove(Utf8Path(landingPath), ec);
            // 目录是本次自测新建的、且探测文件已删干净 -> 把空目录也收走，别在用户项目里留痕。
            if (!dirExistedBefore) std::filesystem::remove(Utf8Path(projectPrefabDir), ec);
        }
    }
    const bool landingOk = landingDirOk && landingInsideProject &&
                           landingWriteOk && plainRoundTripOk;
    LOGI("[PrefabSelftest] landing: dir=%d insideProject=%d write=%d plainRoundTrip=%d -> %s",
        landingDirOk ? 1 : 0, landingInsideProject ? 1 : 0, landingWriteOk ? 1 : 0,
        plainRoundTripOk ? 1 : 0, landingOk ? "PASS" : "FAIL");

    return (ok && treeOk && landingOk) ? 0 : 1;
}

bool DumpSceneState(const std::string& path, int frames, float fps)
{
    return Core::GameplayRuntime::DumpState(path, frames, "render-vulkan", fps);
}

} // namespace Core
