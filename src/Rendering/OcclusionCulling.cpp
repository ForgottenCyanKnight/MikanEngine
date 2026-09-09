// OcclusionCulling.cpp - visibility culling (BVH submesh frustum culling)
// 深度信息在 GPU（z-prepass 已写），正确形态是 GPU HiZ（体素已有基础设施）
#include "Rendering/OcclusionCulling.h"
#include "Rendering/ModelRenderer.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include <unordered_set>

static void QueryTLASRecursive(const ModelBVHData& bvh, int nodeIdx, const glm::mat4& modelMatrix,
                               const std::array<Plane, 6>& frustumPlanes, std::unordered_set<int>& out)
{
    if (nodeIdx < 0 || nodeIdx >= (int)bvh.GetTopLevelNodes().size()) return;
    const auto& node = bvh.GetTopLevelNodes()[nodeIdx];
    AABB world = AABB(node.boundsMin, node.boundsMax).Transform(modelMatrix);
    if (!AABBUtils::IsAABBInFrustum(world, frustumPlanes)) return;   // 剪枝
    if (node.isLeaf && node.subMeshIndex >= 0) { out.insert(node.subMeshIndex); return; }
    QueryTLASRecursive(bvh, node.left, modelMatrix, frustumPlanes, out);
    QueryTLASRecursive(bvh, node.right, modelMatrix, frustumPlanes, out);
}

std::vector<size_t> OcclusionCulling::GetVisibleSubMeshIndices(ModelRenderer* renderer, const glm::mat4& modelMatrix,
                                                               const std::array<Plane, 6>& frustumPlanes,
                                                               const std::vector<ECS::Entity>& allEntities,
                                                               const glm::vec3& cameraPos,
                                                               bool useFrustumCulling,
                                                               ECS::Entity currentEntity)
{
    std::vector<size_t> visibleIndices;
    if (!m_cullingContext) return visibleIndices;
    if (!renderer || !renderer->HasModelLoaded()) {
        return visibleIndices;
    }

    std::vector<AABB> subMeshAABBs = renderer->GetSubMeshAABBs();

    const ModelBVHData& bvhData = renderer->GetBVHData();
    std::unordered_set<int> bvhVisibleSet;
    bool usedBVHForFrustum = false;
    if (useFrustumCulling && bvhData.HasTopLevelBVH()) {
        QueryTLASRecursive(bvhData, bvhData.GetTLASRootIndex(), modelMatrix, frustumPlanes, bvhVisibleSet);
        usedBVHForFrustum = true;
    }

    for (size_t i = 0; i < subMeshAABBs.size(); ++i) {
        AABB worldAABB = subMeshAABBs[i].Transform(modelMatrix);

        if (useFrustumCulling) {
            if (usedBVHForFrustum) {
                if (bvhVisibleSet.find((int)i) == bvhVisibleSet.end()) {
                    continue;   // BVH 剪枝判定不可见
                }
            } else if (!AABBUtils::IsAABBInFrustum(worldAABB, frustumPlanes)) {
                continue;
            }
        }

        visibleIndices.push_back(i);
    }
    return visibleIndices;
}