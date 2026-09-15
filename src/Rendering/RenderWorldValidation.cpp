#include "Rendering/RenderWorld.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

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

    const auto validateFlattenedEntities = [&fail](
        const char* listName,
        const std::vector<ECS::Entity>& flattened,
        const auto& groups) {
        size_t flatIndex = 0;
        for (const auto& group : groups) {
            for (const ECS::Entity entityId : group.entities) {
                if (flatIndex >= flattened.size() || flattened[flatIndex] != entityId) {
                    return fail(std::string(listName) + " does not match its groups");
                }
                ++flatIndex;
            }
        }
        if (flatIndex != flattened.size()) {
            return fail(std::string(listName) + " contains an ungrouped entity");
        }
        return true;
    };

    if (!validateFlattenedEntities("model entity list", modelEntities, modelGroups)) {
        return false;
    }
    if (!validateFlattenedEntities("voxel entity list", voxEntities, voxGroups)) {
        return false;
    }
    const size_t expectedCullingCount = modelEntities.size() + voxEntities.size();
    if (cullingEntities.size() != expectedCullingCount) {
        return fail("culling entity list count does not match model and voxel lists");
    }
    for (size_t index = 0; index < modelEntities.size(); ++index) {
        if (cullingEntities[index] != modelEntities[index]) {
            return fail("culling entity list model prefix is out of sync");
        }
    }
    for (size_t index = 0; index < voxEntities.size(); ++index) {
        if (cullingEntities[modelEntities.size() + index] != voxEntities[index]) {
            return fail("culling entity list voxel suffix is out of sync");
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

