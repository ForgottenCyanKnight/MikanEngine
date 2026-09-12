// SceneDebugRenderer.cpp - editor/debug visualization (AABB/OBB/BVH wireframes)
#include "Rendering/SceneDebugRenderer.h"
#include "Rendering/SceneRenderer.h"   // ModelInstanceGroup / VoxInstanceGroup definitions
#include "Rendering/SceneCollector.h"
#include "Rendering/ModelRenderer.h"
#include "Rendering/VoxRenderer.h"
#include "Rendering/RenderWorld.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "AABB.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kWireSegments = 20;

glm::vec3 TransformPoint(const glm::mat4& matrix, const glm::vec3& point)
{
    return glm::vec3(matrix * glm::vec4(point, 1.0f));
}

// Draw an ellipsoid from the three local half axes. This also handles the
// non-uniform Transform.scale used by Jolt's ScaledShape.
void AddWireEllipsoid(WireframeRenderer& renderer, const glm::mat4& worldMatrix,
                      const glm::vec3& localCenter, const glm::vec3& localRadii,
                      const glm::vec3& color)
{
    if (!std::isfinite(localRadii.x) || !std::isfinite(localRadii.y) ||
        !std::isfinite(localRadii.z) ||
        localRadii.x <= 0.001f || localRadii.y <= 0.001f || localRadii.z <= 0.001f) {
        return;
    }

    for (int plane = 0; plane < 3; ++plane) {
        for (int i = 0; i < kWireSegments; ++i) {
            const float a0 = (static_cast<float>(i) / static_cast<float>(kWireSegments)) * 2.0f * kPi;
            const float a1 = (static_cast<float>(i + 1) / static_cast<float>(kWireSegments)) * 2.0f * kPi;
            const float c0 = std::cos(a0);
            const float s0 = std::sin(a0);
            const float c1 = std::cos(a1);
            const float s1 = std::sin(a1);

            glm::vec3 local0 = localCenter;
            glm::vec3 local1 = localCenter;
            if (plane == 0) {          // XY
                local0 += glm::vec3(c0 * localRadii.x, s0 * localRadii.y, 0.0f);
                local1 += glm::vec3(c1 * localRadii.x, s1 * localRadii.y, 0.0f);
            } else if (plane == 1) {   // XZ
                local0 += glm::vec3(c0 * localRadii.x, 0.0f, s0 * localRadii.z);
                local1 += glm::vec3(c1 * localRadii.x, 0.0f, s1 * localRadii.z);
            } else {                   // YZ
                local0 += glm::vec3(0.0f, c0 * localRadii.y, s0 * localRadii.z);
                local1 += glm::vec3(0.0f, c1 * localRadii.y, s1 * localRadii.z);
            }
            renderer.AddLine(TransformPoint(worldMatrix, local0),
                             TransformPoint(worldMatrix, local1), color);
        }
    }
}

// Jolt's CapsuleShape is aligned to local Y. The editor's size convention is
// diameter on X/Z and cylinder height on Y, matching PhysicsManager::CreateRigidBody.
void AddWireCapsule(WireframeRenderer& renderer, const glm::mat4& worldMatrix,
                    const glm::vec3& localCenter, float radius, float cylinderHalfHeight,
                    const glm::vec3& color)
{
    radius = std::abs(radius);
    cylinderHalfHeight = std::abs(cylinderHalfHeight);
    if (!std::isfinite(radius) || !std::isfinite(cylinderHalfHeight) || radius <= 0.001f)
        return;

    struct Ring {
        float y;
        float radius;
    };
    std::vector<Ring> rings;
    constexpr int kHemisphereRings = 5;
    constexpr int kCylinderRings = 2;

    // Bottom hemisphere, from near the bottom pole to the equator.
    for (int i = 1; i <= kHemisphereRings; ++i) {
        const float theta = -0.5f * kPi +
            (static_cast<float>(i) / static_cast<float>(kHemisphereRings)) * 0.5f * kPi;
        rings.push_back({
            -cylinderHalfHeight + radius * std::sin(theta),
            radius * std::cos(theta)
        });
    }

    // Cylinder side, including the top equator.
    if (cylinderHalfHeight > 0.001f) {
        for (int i = 1; i <= kCylinderRings; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kCylinderRings);
            rings.push_back({
                -cylinderHalfHeight + 2.0f * cylinderHalfHeight * t,
                radius
            });
        }
    }

    // Top hemisphere, excluding its pole (the pole is connected separately).
    for (int i = 1; i < kHemisphereRings; ++i) {
        const float theta = (static_cast<float>(i) / static_cast<float>(kHemisphereRings)) * 0.5f * kPi;
        rings.push_back({
            cylinderHalfHeight + radius * std::sin(theta),
            radius * std::cos(theta)
        });
    }

    std::vector<std::vector<glm::vec3>> ringPoints;
    ringPoints.reserve(rings.size());
    for (const Ring& ring : rings) {
        std::vector<glm::vec3> points;
        points.reserve(kWireSegments);
        for (int i = 0; i < kWireSegments; ++i) {
            const float angle = (static_cast<float>(i) / static_cast<float>(kWireSegments)) * 2.0f * kPi;
            points.push_back(TransformPoint(worldMatrix,
                localCenter + glm::vec3(std::cos(angle) * ring.radius,
                                        ring.y,
                                        std::sin(angle) * ring.radius)));
        }
        for (int i = 0; i < kWireSegments; ++i) {
            renderer.AddLine(points[i], points[(i + 1) % kWireSegments], color);
        }
        ringPoints.push_back(std::move(points));
    }

    for (size_t ringIndex = 1; ringIndex < ringPoints.size(); ++ringIndex) {
        for (int i = 0; i < kWireSegments; ++i) {
            renderer.AddLine(ringPoints[ringIndex - 1][i], ringPoints[ringIndex][i], color);
        }
    }

    if (!ringPoints.empty()) {
        const glm::vec3 bottomPole = TransformPoint(
            worldMatrix, localCenter + glm::vec3(0.0f, -cylinderHalfHeight - radius, 0.0f));
        const glm::vec3 topPole = TransformPoint(
            worldMatrix, localCenter + glm::vec3(0.0f, cylinderHalfHeight + radius, 0.0f));
        for (int i = 0; i < kWireSegments; ++i) {
            renderer.AddLine(bottomPole, ringPoints.front()[i], color);
            renderer.AddLine(topPole, ringPoints.back()[i], color);
        }
    }
}

void AddWireBox(WireframeRenderer& renderer, const glm::mat4& worldMatrix,
                const glm::vec3& localCenter, const glm::vec3& localSize,
                bool oriented, const glm::vec3& color)
{
    const glm::vec3 halfSize = glm::max(glm::abs(localSize) * 0.5f, glm::vec3(0.001f));
    const AABB localAABB(localCenter - halfSize, localCenter + halfSize);
    if (oriented) {
        renderer.AddOBBFromMatrix(localAABB, worldMatrix, color);
    } else {
        renderer.AddAABB(localAABB.Transform(worldMatrix), color);
    }
}

glm::vec3 RigidBodyColor(const ECS::RigidBodyComponent& rigidBody)
{
    if (rigidBody.isTrigger) return glm::vec3(1.0f, 0.15f, 0.8f); // trigger
    switch (rigidBody.type) {
    case ECS::RigidBodyComponent::Type::Static:
        return glm::vec3(0.1f, 1.0f, 0.25f); // static
    case ECS::RigidBodyComponent::Type::Kinematic:
        return glm::vec3(1.0f, 0.8f, 0.05f); // kinematic
    case ECS::RigidBodyComponent::Type::Dynamic:
    default:
        return glm::vec3(1.0f, 0.2f, 0.15f); // dynamic
    }
}

void AddRigidBodyWireframe(
    WireframeRenderer& renderer, ECS::Entity entity,
    const ECS::RigidBodyComponent& rigidBody, const glm::mat4& worldMatrix,
    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers)
{
    const glm::vec3 color = RigidBodyColor(rigidBody);
    switch (rigidBody.shapeType) {
    case ECS::RigidBodyComponent::ShapeType::Box:
        AddWireBox(renderer, worldMatrix, rigidBody.offset, rigidBody.size,
                   true, color);
        break;
    case ECS::RigidBodyComponent::ShapeType::OBB:
        AddWireBox(renderer, worldMatrix, rigidBody.offset, rigidBody.size,
                   true, color);
        break;
    case ECS::RigidBodyComponent::ShapeType::Sphere: {
        const float radius = std::max(0.001f, std::abs(rigidBody.size.x) * 0.5f);
        AddWireEllipsoid(renderer, worldMatrix, rigidBody.offset,
                         glm::vec3(radius), color);
        break;
    }
    case ECS::RigidBodyComponent::ShapeType::Capsule:
        AddWireCapsule(renderer, worldMatrix, rigidBody.offset,
                       rigidBody.size.x * 0.5f, rigidBody.size.y * 0.5f, color);
        break;
    case ECS::RigidBodyComponent::ShapeType::Mesh: {
        // The final convex hull is generated from the model/cache in PhysicsManager.
        // At editor render time the corresponding model AABB/submesh AABBs provide a
        // stable visualization without rebuilding the collision asset every frame.
        const std::string rendererKey = SceneCollector::GetModelRendererKey(entity);
        const auto rendererIt = modelRenderers.find(rendererKey);
        if (rendererIt == modelRenderers.end() || !rendererIt->second ||
            !rendererIt->second->HasModelLoaded()) {
            break;
        }
        const glm::vec3 meshColor(0.75f, 0.2f, 1.0f);
        if (rigidBody.generatePerSubmesh) {
            for (const AABB& localAABB : rendererIt->second->GetSubMeshAABBs()) {
                renderer.AddOBBFromMatrix(localAABB, worldMatrix, meshColor);
            }
        } else {
            renderer.AddOBBFromMatrix(rendererIt->second->GetAABB(), worldMatrix, meshColor);
        }
        break;
    }
    }
}

void AddColliderWireframe(WireframeRenderer& renderer,
                          const ECS::ColliderComponent& collider,
                          const glm::mat4& worldMatrix)
{
    const glm::vec3 color = collider.isTrigger
        ? glm::vec3(1.0f, 0.15f, 0.8f)
        : glm::vec3(0.1f, 0.85f, 1.0f); // collider-only fallback
    switch (collider.type) {
    case ECS::ColliderComponent::Type::Box:
        AddWireBox(renderer, worldMatrix, collider.offset, collider.size,
                   collider.useOBB, color);
        break;
    case ECS::ColliderComponent::Type::Sphere: {
        const float radius = std::max(0.001f, std::abs(collider.size.x) * 0.5f);
        AddWireEllipsoid(renderer, worldMatrix, collider.offset,
                         glm::vec3(radius), color);
        break;
    }
    case ECS::ColliderComponent::Type::Capsule:
        AddWireCapsule(renderer, worldMatrix, collider.offset,
                       collider.size.x * 0.5f, collider.size.y * 0.5f, color);
        break;
    }
}

std::string SnapshotModelRendererKey(const RenderWorldEntity& entity)
{
    if (!entity.hasMesh || entity.mesh.modelPath.empty()) return {};
    if (!entity.hasAnimator && !entity.hasVmdPlayer) return entity.mesh.modelPath;
    return entity.mesh.modelPath + "#entity:" + std::to_string(entity.entity);
}

const ModelRenderer* FindSnapshotModelRenderer(
    const RenderWorldEntity& entity,
    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers)
{
    const std::string key = SnapshotModelRendererKey(entity);
    auto it = modelRenderers.find(key);
    if (it != modelRenderers.end()) return it->second.get();

    // Keep debug visualization useful while a legacy caller still owns a
    // path-keyed renderer for an entity that has just gained animation data.
    if (entity.hasMesh) {
        it = modelRenderers.find(entity.mesh.modelPath);
        if (it != modelRenderers.end()) return it->second.get();
    }
    return nullptr;
}

glm::vec3 RenderRigidBodyColor(const RenderRigidBodyData& rigidBody)
{
    if (rigidBody.isTrigger) return glm::vec3(1.0f, 0.15f, 0.8f);
    switch (rigidBody.type) {
    case RenderRigidBodyType::Static:
        return glm::vec3(0.1f, 1.0f, 0.25f);
    case RenderRigidBodyType::Kinematic:
        return glm::vec3(1.0f, 0.8f, 0.05f);
    case RenderRigidBodyType::Dynamic:
    default:
        return glm::vec3(1.0f, 0.2f, 0.15f);
    }
}

void AddRenderRigidBodyWireframe(
    WireframeRenderer& renderer, const RenderWorldEntity& entity,
    const RenderRigidBodyData& rigidBody, const glm::mat4& worldMatrix,
    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers)
{
    const glm::vec3 color = RenderRigidBodyColor(rigidBody);
    switch (rigidBody.shapeType) {
    case RenderRigidBodyShapeType::Box:
    case RenderRigidBodyShapeType::OBB:
        AddWireBox(renderer, worldMatrix, rigidBody.offset, rigidBody.size,
                   true, color);
        break;
    case RenderRigidBodyShapeType::Sphere: {
        const float radius = std::max(0.001f, std::abs(rigidBody.size.x) * 0.5f);
        AddWireEllipsoid(renderer, worldMatrix, rigidBody.offset,
                         glm::vec3(radius), color);
        break;
    }
    case RenderRigidBodyShapeType::Capsule:
        AddWireCapsule(renderer, worldMatrix, rigidBody.offset,
                       rigidBody.size.x * 0.5f, rigidBody.size.y * 0.5f, color);
        break;
    case RenderRigidBodyShapeType::Mesh: {
        const ModelRenderer* modelRenderer = FindSnapshotModelRenderer(entity, modelRenderers);
        if (!modelRenderer || !modelRenderer->HasModelLoaded()) break;

        const glm::vec3 meshColor(0.75f, 0.2f, 1.0f);
        if (rigidBody.generatePerSubmesh) {
            for (const AABB& localAABB : modelRenderer->GetSubMeshAABBs()) {
                renderer.AddOBBFromMatrix(localAABB, worldMatrix, meshColor);
            }
        } else {
            renderer.AddOBBFromMatrix(modelRenderer->GetAABB(), worldMatrix, meshColor);
        }
        break;
    }
    }
}

void AddRenderColliderWireframe(WireframeRenderer& renderer,
                                const RenderColliderData& collider,
                                const glm::mat4& worldMatrix)
{
    const glm::vec3 color = collider.isTrigger
        ? glm::vec3(1.0f, 0.15f, 0.8f)
        : glm::vec3(0.1f, 0.85f, 1.0f);
    switch (collider.type) {
    case RenderColliderType::Box:
        AddWireBox(renderer, worldMatrix, collider.offset, collider.size,
                   collider.useOBB, color);
        break;
    case RenderColliderType::Sphere: {
        const float radius = std::max(0.001f, std::abs(collider.size.x) * 0.5f);
        AddWireEllipsoid(renderer, worldMatrix, collider.offset,
                         glm::vec3(radius), color);
        break;
    }
    case RenderColliderType::Capsule:
        AddWireCapsule(renderer, worldMatrix, collider.offset,
                       collider.size.x * 0.5f, collider.size.y * 0.5f, color);
        break;
    }
}

} // namespace

// Collect all camera entities under an entity subtree (helper for BVH wireframe visibility)
void SceneDebugRenderer::CollectCameraEntities(ECS::Entity entity, std::vector<ECS::Entity>& cameraEntities)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::CameraComponent>(entity))
        cameraEntities.push_back(entity);

    auto children = sceneECS.GetChildren(entity);
    for (const auto& child : children)
        CollectCameraEntities(child, cameraEntities);
}

void SceneDebugRenderer::CollectAABBs(ECS::Entity entity,
                                      const std::unordered_map<std::string, ModelInstanceGroup>& modelGroups,
                                      const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers,
                                      const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers)
{
    (void)modelGroups; (void)voxRenderers;
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::MeshComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible && (render.showAABB || render.showOBB)) {
            auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);

            if ((mesh.type == ECS::MeshType::Model || mesh.type == ECS::MeshType::Plane) && !mesh.modelPath.empty()) {
                auto rendererIt = modelRenderers.find(SceneCollector::GetModelRendererKey(entity));
                if (rendererIt != modelRenderers.end() && rendererIt->second && rendererIt->second->HasModelLoaded()) {
                    std::vector<AABB> subMeshAABBs = rendererIt->second->GetSubMeshAABBs();
                    glm::mat4 modelMatrix = transform.GetModelMatrix();

                    if (render.showOBB) {
                        for (const auto& localAABB : subMeshAABBs)
                            m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, glm::vec3(0.0f, 1.0f, 0.0f)); // green
                    } else if (render.showAABB) {
                        for (const auto& localAABB : subMeshAABBs) {
                            AABB worldAABB = localAABB.Transform(modelMatrix);
                            m_WireframeRenderer.AddAABB(worldAABB, glm::vec3(0.0f, 1.0f, 0.0f)); // green
                        }
                    }
                }
            }
        }
    }

    auto children = sceneECS.GetChildren(entity);
    for (const auto& child : children)
        CollectAABBs(child, modelGroups, modelRenderers, voxRenderers);
}

void SceneDebugRenderer::CollectVoxAABBs(ECS::Entity entity,
                                         const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::VoxModelComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible && (render.showAABB || render.showOBB)) {
            auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);

            if (!vox.voxPath.empty()) {
                auto rendererIt = voxRenderers.find(vox.voxPath);
                if (rendererIt != voxRenderers.end() && rendererIt->second && rendererIt->second->HasLoaded()) {
                    auto& voxRenderer = rendererIt->second;
                    glm::vec3 minBounds = voxRenderer->GetMinBounds();
                    glm::vec3 maxBounds = voxRenderer->GetMaxBounds();

                    AABB localAABB;
                    localAABB.min = minBounds;
                    localAABB.max = maxBounds;

                    glm::mat4 modelMatrix = transform.GetModelMatrix();

                    if (render.showOBB) {
                        m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, glm::vec3(1.0f, 0.5f, 0.0f)); // orange
                    } else if (render.showAABB) {
                        AABB worldAABB = localAABB.Transform(modelMatrix);
                        m_WireframeRenderer.AddAABB(worldAABB, glm::vec3(1.0f, 0.5f, 0.0f)); // orange
                    }
                }
            }
        }
    }

    auto children = sceneECS.GetChildren(entity);
    for (const auto& child : children)
        CollectVoxAABBs(child, voxRenderers);
}

void SceneDebugRenderer::CollectAABBs(
    const RenderWorld& world,
    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers)
{
    for (const RenderWorldEntity& entity : world.entities) {
        if (!entity.visible || !entity.hasTransform || !entity.hasMesh ||
            !entity.hasRenderFlags || !entity.render.visible ||
            !(entity.mesh.type == RenderMeshType::Model ||
              entity.mesh.type == RenderMeshType::Plane) ||
            entity.mesh.modelPath.empty() ||
            !(entity.render.showAABB || entity.render.showOBB)) {
            continue;
        }

        const ModelRenderer* renderer = FindSnapshotModelRenderer(entity, modelRenderers);
        if (!renderer || !renderer->HasModelLoaded()) continue;

        const glm::mat4& modelMatrix = entity.transform.worldMatrix;
        for (const AABB& localAABB : renderer->GetSubMeshAABBs()) {
            if (entity.render.showOBB) {
                m_WireframeRenderer.AddOBBFromMatrix(
                    localAABB, modelMatrix, glm::vec3(0.0f, 1.0f, 0.0f));
            } else if (entity.render.showAABB) {
                m_WireframeRenderer.AddAABB(
                    localAABB.Transform(modelMatrix), glm::vec3(0.0f, 1.0f, 0.0f));
            }
        }
    }
}

void SceneDebugRenderer::CollectVoxAABBs(
    const RenderWorld& world,
    const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers)
{
    for (const RenderWorldEntity& entity : world.entities) {
        if (!entity.visible || !entity.hasTransform || !entity.hasVoxel ||
            !entity.hasRenderFlags || !entity.render.visible ||
            entity.voxel.voxPath.empty() ||
            !(entity.render.showAABB || entity.render.showOBB)) {
            continue;
        }

        auto rendererIt = voxRenderers.find(entity.voxel.voxPath);
        if (rendererIt == voxRenderers.end() || !rendererIt->second ||
            !rendererIt->second->HasLoaded()) {
            continue;
        }

        AABB localAABB;
        localAABB.min = rendererIt->second->GetMinBounds();
        localAABB.max = rendererIt->second->GetMaxBounds();
        const glm::mat4& modelMatrix = entity.transform.worldMatrix;
        if (entity.render.showOBB) {
            m_WireframeRenderer.AddOBBFromMatrix(
                localAABB, modelMatrix, glm::vec3(1.0f, 0.5f, 0.0f));
        } else if (entity.render.showAABB) {
            m_WireframeRenderer.AddAABB(
                localAABB.Transform(modelMatrix), glm::vec3(1.0f, 0.5f, 0.0f));
        }
    }
}

void SceneDebugRenderer::CollectBVH(ECS::Entity entity, const glm::vec3& cameraPos,
                                    const glm::mat4& effectiveCullView, const glm::mat4& effectiveCullProj,
                                    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers,
                                    const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    std::vector<ECS::Entity> cameraEntities;
    for (const auto& rootEntity : sceneECS.GetRootEntities())
        CollectCameraEntities(rootEntity, cameraEntities);

    bool showBVH = false;
    for (const auto& camEntity : cameraEntities) {
        auto& camera = coordinator.GetComponent<ECS::CameraComponent>(camEntity);
        if (camera.showBVHWireframe) { showBVH = true; break; }
    }
    if (!showBVH) return;

    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::MeshComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible) {
            auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);

            if ((mesh.type == ECS::MeshType::Model || mesh.type == ECS::MeshType::Plane) && !mesh.modelPath.empty()) {
                auto rendererIt = modelRenderers.find(SceneCollector::GetModelRendererKey(entity));
                if (rendererIt != modelRenderers.end() && rendererIt->second && rendererIt->second->HasBVH()) {
                    glm::mat4 modelMatrix = transform.GetModelMatrix();

                    if (rendererIt->second->HasTopLevelBVH()) {
                        std::vector<AABB> topLevelBounds = rendererIt->second->GetTopLevelBVHNodeBounds();
                        for (const auto& localAABB : topLevelBounds)
                            m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, glm::vec3(1.0f, 0.8f, 0.0f)); // yellow
                    }

                    const auto& bvhData = rendererIt->second->GetBVHData();
                    size_t blasCount = bvhData.GetSubMeshCount();
                    for (size_t blasIdx = 0; blasIdx < blasCount; blasIdx++) {
                        std::vector<AABB> bvhBounds = rendererIt->second->GetBVHNodeBounds(blasIdx);
                        for (const auto& localAABB : bvhBounds)
                            m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, glm::vec3(0.0f, 0.5f, 1.0f)); // cyan
                    }
                }
            }
        }
    }

    // VoxRenderer BVH (green, depth-shaded)
    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::VoxModelComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible) {
            auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);

            auto rendererIt = voxRenderers.find(vox.voxPath);
            if (rendererIt != voxRenderers.end() && rendererIt->second && rendererIt->second->HasBVH()) {
                glm::mat4 modelMatrix = transform.GetModelMatrix();
                const auto& bvh = rendererIt->second->GetBVH();
                const auto& nodes = bvh.getNodes();

                for (const auto& node : nodes) {
                    glm::vec3 center = (node.boundsMin + node.boundsMax) * 0.5f;
                    glm::vec3 halfSize = (node.boundsMax - node.boundsMin) * 0.5f;
                    center.z = -center.z; // BVH vs vox model Z convention flip

                    AABB localAABB(center - halfSize, center + halfSize);
                    float depthFactor = 1.0f - (node.depth / 30.0f);
                    glm::vec3 color = glm::vec3(0.0f, depthFactor * 0.8f, depthFactor * 0.3f);
                    m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, color);
                }
            }
        }
    }

    auto children = sceneECS.GetChildren(entity);
    for (const auto& child : children)
        CollectBVH(child, cameraPos, effectiveCullView, effectiveCullProj, modelRenderers, voxRenderers);
}

void SceneDebugRenderer::CollectBVH(
    const RenderWorld& world, const glm::vec3& cameraPos,
    const glm::mat4& effectiveCullView, const glm::mat4& effectiveCullProj,
    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers,
    const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers)
{
    (void)cameraPos;
    (void)effectiveCullView;
    (void)effectiveCullProj;

    bool showBVH = false;
    for (const RenderCameraData& camera : world.cameras) {
        if (camera.showBVHWireframe) {
            showBVH = true;
            break;
        }
    }
    if (!showBVH) return;

    for (const RenderWorldEntity& entity : world.entities) {
        if (!entity.visible || !entity.hasTransform || !entity.hasRenderFlags ||
            !entity.render.visible) {
            continue;
        }

        const glm::mat4& modelMatrix = entity.transform.worldMatrix;
        if (entity.hasMesh &&
            (entity.mesh.type == RenderMeshType::Model ||
             entity.mesh.type == RenderMeshType::Plane) &&
            !entity.mesh.modelPath.empty()) {
            const ModelRenderer* renderer = FindSnapshotModelRenderer(entity, modelRenderers);
            if (renderer && renderer->HasBVH()) {
                if (renderer->HasTopLevelBVH()) {
                    for (const AABB& localAABB : renderer->GetTopLevelBVHNodeBounds()) {
                        m_WireframeRenderer.AddOBBFromMatrix(
                            localAABB, modelMatrix, glm::vec3(1.0f, 0.8f, 0.0f));
                    }
                }

                const auto& bvhData = renderer->GetBVHData();
                for (size_t blasIndex = 0; blasIndex < bvhData.GetSubMeshCount(); ++blasIndex) {
                    for (const AABB& localAABB : renderer->GetBVHNodeBounds(blasIndex)) {
                        m_WireframeRenderer.AddOBBFromMatrix(
                            localAABB, modelMatrix, glm::vec3(0.0f, 0.5f, 1.0f));
                    }
                }
            }
        }

        // VoxRenderer BVH (green, depth-shaded).
        if (entity.hasVoxel && !entity.voxel.voxPath.empty()) {
            auto rendererIt = voxRenderers.find(entity.voxel.voxPath);
            if (rendererIt == voxRenderers.end() || !rendererIt->second ||
                !rendererIt->second->HasBVH()) {
                continue;
            }

            const auto& nodes = rendererIt->second->GetBVH().getNodes();
            for (const auto& node : nodes) {
                glm::vec3 center = (node.boundsMin + node.boundsMax) * 0.5f;
                const glm::vec3 halfSize = (node.boundsMax - node.boundsMin) * 0.5f;
                center.z = -center.z;

                const AABB localAABB(center - halfSize, center + halfSize);
                const float depthFactor = 1.0f - (node.depth / 30.0f);
                const glm::vec3 color(0.0f, depthFactor * 0.8f, depthFactor * 0.3f);
                m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, color);
            }
        }
    }
}

void SceneDebugRenderer::CollectCollisionWireframes(
    const std::vector<ECS::Entity>& cameraEntities,
    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    bool showCollisionWireframe = false;
    for (const ECS::Entity cameraEntity : cameraEntities) {
        if (coordinator.HasComponent<ECS::CameraComponent>(cameraEntity) &&
            coordinator.GetComponent<ECS::CameraComponent>(cameraEntity).showCollisionWireframe) {
            showCollisionWireframe = true;
            break;
        }
    }
    if (!showCollisionWireframe) return;

    std::function<void(ECS::Entity)> visit = [&](ECS::Entity entity) {
        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            const glm::mat4 worldMatrix = sceneECS.GetWorldMatrix(entity);

            // A RigidBodyComponent is authoritative when both components exist;
            // this prevents drawing a stale ColliderComponent over the real Jolt body.
            if (coordinator.HasComponent<ECS::RigidBodyComponent>(entity)) {
                AddRigidBodyWireframe(
                    m_WireframeRenderer, entity,
                    coordinator.GetComponent<ECS::RigidBodyComponent>(entity),
                    worldMatrix, modelRenderers);
            } else if (coordinator.HasComponent<ECS::ColliderComponent>(entity)) {
                AddColliderWireframe(
                    m_WireframeRenderer,
                    coordinator.GetComponent<ECS::ColliderComponent>(entity),
                    worldMatrix);
            }
        }

        for (const ECS::Entity child : sceneECS.GetChildren(entity)) {
            visit(child);
        }
    };

    for (const ECS::Entity root : sceneECS.GetRootEntities()) {
        visit(root);
    }
}

void SceneDebugRenderer::CollectCollisionWireframes(
    const RenderWorld& world,
    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers)
{
    bool showCollisionWireframe = false;
    for (const RenderCameraData& camera : world.cameras) {
        if (camera.showCollisionWireframe) {
            showCollisionWireframe = true;
            break;
        }
    }
    if (!showCollisionWireframe) return;

    for (const RenderWorldEntity& entity : world.entities) {
        if (!entity.hasTransform) continue;

        if (entity.hasRigidBody) {
            AddRenderRigidBodyWireframe(
                m_WireframeRenderer, entity, entity.rigidBody,
                entity.transform.worldMatrix, modelRenderers);
        } else if (entity.hasCollider) {
            AddRenderColliderWireframe(
                m_WireframeRenderer, entity.collider,
                entity.transform.worldMatrix);
        }
    }
}
