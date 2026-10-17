#include "Rendering/SceneCollector.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

std::string SceneCollector::GetModelRendererKey(ECS::Entity entity)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::MeshComponent>(entity)) return {};

    const auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
    if (mesh.modelPath.empty()) return {};

    // ModelRenderer stores animation time/bones internally. An entity with an
    // AnimatorComponent therefore cannot share the path-only renderer with a
    // different animated entity, even when both load the same asset.
    auto ext=std::filesystem::path(mesh.modelPath).extension().string();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if (ext != ".pmx" && !coordinator.HasComponent<ECS::AnimatorComponent>(entity) &&
        !coordinator.HasComponent<ECS::VmdPlayerComponent>(entity)) {
        return mesh.modelPath;
    }
    return mesh.modelPath + "#entity:" + std::to_string(entity);
}
