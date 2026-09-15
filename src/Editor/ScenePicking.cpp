#include "Editor/ScenePicking.h"

#include "AABB.h"
#include "Rendering/ModelLoader.h"
#include "Rendering/ModelRenderer.h"
#include "Rendering/RenderWorld.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/VoxRenderer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace Editor {

namespace {

constexpr std::size_t kInvalidSubMesh = static_cast<std::size_t>(-1);
constexpr std::size_t kMaxTrianglesForClickPick = 200000;
constexpr float kLightMarkerHitRadiusPixels = 20.0f;
constexpr float kRayEpsilon = 0.000001f;

bool IsFiniteVector(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsPickable(const RenderWorldEntity* entity)
{
    return entity != nullptr && entity->visible && entity->hasTransform &&
        (!entity->hasRenderFlags || entity->render.visible);
}

bool IntersectAABB(const glm::vec3& rayOrigin,
                  const glm::vec3& rayDirection,
                  const AABB& bounds,
                  float& distance)
{
    if (!IsFiniteVector(bounds.min) || !IsFiniteVector(bounds.max) ||
        glm::any(glm::greaterThan(bounds.min, bounds.max))) {
        return false;
    }

    float nearDistance = 0.0f;
    float farDistance = std::numeric_limits<float>::infinity();
    const glm::vec3 originValues = rayOrigin;
    const glm::vec3 directionValues = rayDirection;

    for (int axis = 0; axis < 3; ++axis) {
        const float origin = originValues[axis];
        const float direction = directionValues[axis];
        const float minValue = bounds.min[axis];
        const float maxValue = bounds.max[axis];

        if (std::abs(direction) <= kRayEpsilon) {
            if (origin < minValue || origin > maxValue) {
                return false;
            }
            continue;
        }

        const float inverseDirection = 1.0f / direction;
        float axisNear = (minValue - origin) * inverseDirection;
        float axisFar = (maxValue - origin) * inverseDirection;
        if (axisNear > axisFar) {
            std::swap(axisNear, axisFar);
        }

        nearDistance = std::max(nearDistance, axisNear);
        farDistance = std::min(farDistance, axisFar);
        if (nearDistance > farDistance) {
            return false;
        }
    }

    distance = nearDistance;
    return std::isfinite(distance) && farDistance >= 0.0f;
}

bool IntersectTriangle(const glm::vec3& rayOrigin,
                       const glm::vec3& rayDirection,
                       const glm::vec3& v0,
                       const glm::vec3& v1,
                       const glm::vec3& v2,
                       float& distance)
{
    const glm::vec3 edge1 = v1 - v0;
    const glm::vec3 edge2 = v2 - v0;
    const glm::vec3 p = glm::cross(rayDirection, edge2);
    const float determinant = glm::dot(edge1, p);
    if (std::abs(determinant) <= kRayEpsilon) {
        return false;
    }

    const float inverseDeterminant = 1.0f / determinant;
    const glm::vec3 originToVertex = rayOrigin - v0;
    const float u = glm::dot(originToVertex, p) * inverseDeterminant;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }

    const glm::vec3 q = glm::cross(originToVertex, edge1);
    const float v = glm::dot(rayDirection, q) * inverseDeterminant;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }

    distance = glm::dot(edge2, q) * inverseDeterminant;
    return distance >= 0.0f && std::isfinite(distance);
}

bool ProjectWorldPoint(const glm::mat4& view,
                       const glm::mat4& projection,
                       const glm::vec3& worldPosition,
                       const glm::vec2& viewportMin,
                       const glm::vec2& viewportSize,
                       glm::vec2& screenPosition)
{
    const glm::vec4 clipPosition = projection * view * glm::vec4(worldPosition, 1.0f);
    if (clipPosition.w <= 0.0001f || viewportSize.x <= 1.0f || viewportSize.y <= 1.0f) {
        return false;
    }

    const glm::vec3 ndc = glm::vec3(clipPosition) / clipPosition.w;
    if (ndc.z < -1.0f || ndc.z > 1.0f) {
        return false;
    }

    screenPosition.x = viewportMin.x + (ndc.x * 0.5f + 0.5f) * viewportSize.x;
    screenPosition.y = viewportMin.y + (-ndc.y * 0.5f + 0.5f) * viewportSize.y;
    return true;
}

void ConsiderHit(std::optional<ScenePickHit>& bestHit,
                 ECS::Entity entity,
                 float distance,
                 std::size_t subMeshIndex = kInvalidSubMesh)
{
    if (distance < 0.0f || !std::isfinite(distance)) {
        return;
    }
    if (!bestHit.has_value() || distance < bestHit->distance) {
        bestHit = ScenePickHit{entity, distance, subMeshIndex};
    }
}

bool GetPrimitiveBounds(RenderMeshType type, AABB& bounds)
{
    switch (type) {
        case RenderMeshType::Cube:
        case RenderMeshType::Sphere:
        case RenderMeshType::Cylinder:
        case RenderMeshType::Cone:
        case RenderMeshType::Capsule:
        case RenderMeshType::Torus:
        case RenderMeshType::Pyramid:
            bounds = AABB(glm::vec3(-0.5f), glm::vec3(0.5f));
            return true;
        case RenderMeshType::Plane:
            bounds = AABB(glm::vec3(-0.5f, -0.025f, -0.5f),
                          glm::vec3(0.5f, 0.025f, 0.5f));
            return true;
        case RenderMeshType::None:
        case RenderMeshType::Model:
            return false;
    }
    return false;
}

std::optional<ScenePickHit> IntersectStaticModel(
    const ScenePickRay& ray,
    ECS::Entity entity,
    const ModelRenderer& renderer,
    const glm::mat4& modelMatrix)
{
    // Animated meshes are rendered from CPU-skinned vertex buffers. The raw
    // MeshData is the bind pose, so a triangle hit would be misleading; the
    // already-correct model AABB remains the safe fallback for P0.
    if (renderer.HasSkinning()) {
        return std::nullopt;
    }

    const MeshData& meshData = renderer.GetMeshData();
    std::size_t triangleCount = 0;
    for (const SubMesh& subMesh : meshData.subMeshes) {
        triangleCount += subMesh.indices.size() / 3;
    }
    if (triangleCount == 0 || triangleCount > kMaxTrianglesForClickPick) {
        return std::nullopt;
    }

    const float determinant = glm::determinant(modelMatrix);
    if (!std::isfinite(determinant) || std::abs(determinant) <= kRayEpsilon) {
        return std::nullopt;
    }

    const glm::mat4 inverseModel = glm::inverse(modelMatrix);
    const glm::vec4 localOrigin4 = inverseModel * glm::vec4(ray.origin, 1.0f);
    const glm::vec4 localDirection4 = inverseModel * glm::vec4(ray.direction, 0.0f);
    const glm::vec3 localOrigin = glm::vec3(localOrigin4);
    const float localDirectionLength = glm::length(glm::vec3(localDirection4));
    if (!std::isfinite(localDirectionLength) || localDirectionLength <= kRayEpsilon) {
        return std::nullopt;
    }
    const glm::vec3 localDirection = glm::vec3(localDirection4) / localDirectionLength;

    const auto& subMeshBounds = renderer.GetBVHData().subMeshAABBs;
    float closestDistance = std::numeric_limits<float>::infinity();
    std::size_t closestSubMesh = kInvalidSubMesh;

    for (std::size_t subMeshIndex = 0;
         subMeshIndex < meshData.subMeshes.size();
         ++subMeshIndex) {
        const SubMesh& subMesh = meshData.subMeshes[subMeshIndex];
        if (subMesh.indices.size() < 3 || subMesh.vertices.empty()) {
            continue;
        }

        if (subMeshIndex < subMeshBounds.size()) {
            float localBoundsDistance = 0.0f;
            if (!IntersectAABB(localOrigin, localDirection,
                               subMeshBounds[subMeshIndex], localBoundsDistance)) {
                continue;
            }
        }

        for (std::size_t index = 0; index + 2 < subMesh.indices.size(); index += 3) {
            const unsigned int i0 = subMesh.indices[index];
            const unsigned int i1 = subMesh.indices[index + 1];
            const unsigned int i2 = subMesh.indices[index + 2];
            if (i0 >= subMesh.vertices.size() ||
                i1 >= subMesh.vertices.size() ||
                i2 >= subMesh.vertices.size()) {
                continue;
            }

            float localDistance = 0.0f;
            if (!IntersectTriangle(localOrigin, localDirection,
                                   subMesh.vertices[i0].Position,
                                   subMesh.vertices[i1].Position,
                                   subMesh.vertices[i2].Position,
                                   localDistance)) {
                continue;
            }

            const glm::vec3 localHit = localOrigin + localDirection * localDistance;
            const glm::vec3 worldHit = glm::vec3(modelMatrix * glm::vec4(localHit, 1.0f));
            const float worldDistance = glm::dot(worldHit - ray.origin, ray.direction);
            if (worldDistance >= 0.0f && worldDistance < closestDistance) {
                closestDistance = worldDistance;
                closestSubMesh = subMeshIndex;
            }
        }
    }

    if (!std::isfinite(closestDistance)) {
        return std::nullopt;
    }
    return ScenePickHit{entity, closestDistance, closestSubMesh};
}

} // namespace

std::optional<ScenePickHit> PickSceneEntity(
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec2& mousePosition,
    const glm::vec2& viewportMin,
    const glm::vec2& viewportSize,
    const RenderWorld& renderWorld,
    SceneRenderer& sceneRenderer)
{
    if (viewportSize.x <= 1.0f || viewportSize.y <= 1.0f ||
        mousePosition.x < viewportMin.x || mousePosition.y < viewportMin.y ||
        mousePosition.x > viewportMin.x + viewportSize.x ||
        mousePosition.y > viewportMin.y + viewportSize.y) {
        return std::nullopt;
    }

    const glm::mat4 inverseView = glm::inverse(view);
    const glm::vec3 rayOrigin = glm::vec3(inverseView[3]);
    const glm::vec4 clipPosition(
        2.0f * (mousePosition.x - viewportMin.x) / viewportSize.x - 1.0f,
        1.0f - 2.0f * (mousePosition.y - viewportMin.y) / viewportSize.y,
        1.0f,
        1.0f);
    const glm::vec4 farWorld4 = glm::inverse(projection * view) * clipPosition;
    if (!std::isfinite(farWorld4.w) || std::abs(farWorld4.w) <= kRayEpsilon) {
        return std::nullopt;
    }
    const glm::vec3 farWorld = glm::vec3(farWorld4) / farWorld4.w;
    const glm::vec3 directionVector = farWorld - rayOrigin;
    const float directionLength = glm::length(directionVector);
    if (!std::isfinite(directionLength) || directionLength <= kRayEpsilon) {
        return std::nullopt;
    }
    const ScenePickRay ray{rayOrigin, directionVector / directionLength};

    // Light markers are an editor overlay. Give them priority when the click
    // is on the drawn glyph, even when a mesh is behind the marker.
    std::optional<ScenePickHit> lightMarkerHit;
    float closestMarkerPixels = std::numeric_limits<float>::infinity();
    for (const RenderLightData& light : renderWorld.lights) {
        const RenderWorldEntity* entityData = renderWorld.Find(light.entity);
        if (!IsPickable(entityData)) {
            continue;
        }

        const glm::vec3 worldPosition = glm::vec3(entityData->transform.worldMatrix[3]);
        glm::vec2 screenPosition;
        if (!ProjectWorldPoint(view, projection, worldPosition,
                               viewportMin, viewportSize, screenPosition)) {
            continue;
        }

        const float pixelDistance = glm::distance(mousePosition, screenPosition);
        if (pixelDistance > kLightMarkerHitRadiusPixels ||
            pixelDistance >= closestMarkerPixels) {
            continue;
        }

        const float worldDistance = glm::dot(worldPosition - ray.origin, ray.direction);
        if (worldDistance < 0.0f || !std::isfinite(worldDistance)) {
            continue;
        }
        closestMarkerPixels = pixelDistance;
        lightMarkerHit = ScenePickHit{light.entity, worldDistance, kInvalidSubMesh};
    }
    if (lightMarkerHit.has_value()) {
        return lightMarkerHit;
    }

    std::optional<ScenePickHit> bestHit;
    std::unordered_set<ECS::Entity> processedEntities;
    processedEntities.reserve(renderWorld.modelEntities.size() + renderWorld.voxEntities.size());

    for (const RenderModelGroup& group : renderWorld.modelGroups) {
        ModelRenderer* renderer = sceneRenderer.GetModelRenderer(group.rendererKey);
        if (renderer == nullptr || !renderer->HasModelLoaded()) {
            continue;
        }

        for (const ECS::Entity entity : group.entities) {
            if (!processedEntities.insert(entity).second) {
                continue;
            }
            const RenderWorldEntity* entityData = renderWorld.Find(entity);
            if (!IsPickable(entityData)) {
                continue;
            }

            const glm::mat4 modelMatrix = entityData->transform.worldMatrix;
            const AABB worldBounds = renderer->GetAABB().Transform(modelMatrix);
            float boundsDistance = 0.0f;
            if (!IntersectAABB(ray.origin, ray.direction, worldBounds, boundsDistance)) {
                continue;
            }

            const std::optional<ScenePickHit> exactHit = IntersectStaticModel(
                ray, entity, *renderer, modelMatrix);
            if (exactHit.has_value()) {
                ConsiderHit(bestHit, exactHit->entity, exactHit->distance,
                            exactHit->subMeshIndex);
            } else {
                ConsiderHit(bestHit, entity, boundsDistance);
            }
        }
    }

    for (const RenderVoxGroup& group : renderWorld.voxGroups) {
        VoxRenderer* renderer = sceneRenderer.GetVoxRenderer(group.voxPath);
        for (const ECS::Entity entity : group.entities) {
            if (!processedEntities.insert(entity).second) {
                continue;
            }
            const RenderWorldEntity* entityData = renderWorld.Find(entity);
            if (!IsPickable(entityData)) {
                continue;
            }

            AABB localBounds(glm::vec3(-0.5f), glm::vec3(0.5f));
            if (renderer != nullptr && renderer->HasLoaded()) {
                localBounds = AABB(renderer->GetMinBounds(), renderer->GetMaxBounds());
            }
            const glm::mat4 renderMatrix = entityData->transform.worldMatrix *
                glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));
            float boundsDistance = 0.0f;
            if (IntersectAABB(ray.origin, ray.direction,
                              localBounds.Transform(renderMatrix), boundsDistance)) {
                ConsiderHit(bestHit, entity, boundsDistance);
            }
        }
    }

    // Built-in primitive meshes and collider-only editor objects do not enter
    // RenderWorld model groups yet. Their compact bounds still make them
    // selectable and keep the interaction consistent with the hierarchy.
    for (const RenderWorldEntity& entityData : renderWorld.entities) {
        if (!processedEntities.insert(entityData.entity).second ||
            !IsPickable(&entityData) || entityData.hasLight) {
            continue;
        }

        AABB localBounds;
        bool hasBounds = false;
        if (entityData.hasMesh) {
            hasBounds = GetPrimitiveBounds(entityData.mesh.type, localBounds);
        }
        if (!hasBounds && entityData.hasCollider) {
            const glm::vec3 halfSize = glm::max(
                glm::abs(entityData.collider.size) * 0.5f,
                glm::vec3(0.025f));
            localBounds = AABB(entityData.collider.offset - halfSize,
                               entityData.collider.offset + halfSize);
            hasBounds = true;
        }
        if (!hasBounds) {
            continue;
        }

        float boundsDistance = 0.0f;
        if (IntersectAABB(ray.origin, ray.direction,
                          localBounds.Transform(entityData.transform.worldMatrix),
                          boundsDistance)) {
            ConsiderHit(bestHit, entityData.entity, boundsDistance);
        }
    }

    return bestHit;
}

} // namespace Editor
