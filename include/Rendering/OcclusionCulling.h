#pragma once
// OcclusionCulling.h - visibility culling (quadtree + Hi-Z occlusion + BVH submesh)
// Split out of SceneRenderer: answers "is this invisible?" without drawing anything.
#include "Platform/Export.h"
#include <vector>
#include <string>
#include <functional>
#include <array>
#include "ECS/Types.h"
#include "QuadTreeCulling.h"
#include "AABB.h"

class ModelRenderer;
struct Plane;

class MIKAN_API OcclusionCulling {
public:
    void SetCullingContext(Culling::CullingContext* ctx) { m_cullingContext = ctx; }
    bool HasCullingContext() const { return m_cullingContext != nullptr; }   // 2026-08-09 z-prepass 剔除前判空

    // Per-submesh visibility (frustum + BVH) for a model renderer
    // 2026-08-09：CPU 遮挡剔除已清理（精度不保守 + 深度信息在 GPU；未来用 GPU HiZ）
    std::vector<size_t> GetVisibleSubMeshIndices(ModelRenderer* renderer, const glm::mat4& modelMatrix,
                                                 const std::array<Plane, 6>& frustumPlanes,
                                                 const std::vector<ECS::Entity>& allEntities,
                                                 const glm::vec3& cameraPos,
                                                 bool useFrustumCulling,
                                                 ECS::Entity currentEntity);

private:
    Culling::CullingContext* m_cullingContext = nullptr;
};