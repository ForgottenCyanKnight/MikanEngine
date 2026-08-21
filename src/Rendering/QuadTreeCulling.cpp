#include "QuadTreeCulling.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "SceneRenderer.h"
#include "Core/EngineGlobal.h"
#include <iostream>

// 声明全局的 g_SceneRenderer
extern SceneRenderer g_SceneRenderer;

namespace Culling {

FrustumQuadTree::FrustumQuadTree(const AABB& rootBounds, const std::vector<AABB>* allAABBs, 
                                 size_t maxDepth, size_t maxEntitiesPerNode)
    : m_MaxDepth(maxDepth), m_MaxEntitiesPerNode(maxEntitiesPerNode), m_AllAABBs(allAABBs) {
    m_Root = std::make_unique<QuadTreeNode>(rootBounds);
}

void FrustumQuadTree::Insert(size_t entityIndex) {
    if (!m_AllAABBs || entityIndex >= m_AllAABBs->size()) return;
    InsertRecursive(m_Root.get(), entityIndex, 0);
}

void FrustumQuadTree::Clear() {
    if (m_Root) {
        m_Root = std::make_unique<QuadTreeNode>(m_Root->bounds);
    }
}

void FrustumQuadTree::InsertRecursive(QuadTreeNode* node, size_t entityIndex, size_t depth) {
    const AABB& aabb = (*m_AllAABBs)[entityIndex];
    
    // 如果是叶子节点
        if (node->isLeaf) {
            // 如果还有空间或达到最大深度，直接添加
            if (node->entityIndices.size() < m_MaxEntitiesPerNode || depth >= m_MaxDepth) {
                node->entityIndices.push_back(entityIndex);
                return;
            }
            
            // 需要分割
            Subdivide(node);
        
        // 重新分配当前节点中的实体到子节点
        std::vector<size_t> oldEntities = std::move(node->entityIndices);
        node->entityIndices.clear();
        
        for (size_t idx : oldEntities) {
            const AABB& oldAABB = (*m_AllAABBs)[idx];
            int quadrant = GetQuadrant(node->bounds, oldAABB);
            if (quadrant >= 0 && quadrant < 4 && node->children[quadrant]) {
                InsertRecursive(node->children[quadrant].get(), idx, depth + 1);
            } else {
                // 如果无法放入子节点，保留在当前节点
                node->entityIndices.push_back(idx);
            }
        }
        
        // 现在递归插入新实体
        int quadrant = GetQuadrant(node->bounds, aabb);
        if (quadrant >= 0 && quadrant < 4 && node->children[quadrant]) {
            InsertRecursive(node->children[quadrant].get(), entityIndex, depth + 1);
        } else {
            node->entityIndices.push_back(entityIndex);
        }
    } else {
        // 非叶子节点，直接递归到子节点
        int quadrant = GetQuadrant(node->bounds, aabb);
        if (quadrant >= 0 && quadrant < 4 && node->children[quadrant]) {
            InsertRecursive(node->children[quadrant].get(), entityIndex, depth + 1);
        } else {
            node->entityIndices.push_back(entityIndex);
        }
    }
}

void FrustumQuadTree::Subdivide(QuadTreeNode* node) {
    node->isLeaf = false;
    
    for (int i = 0; i < 4; ++i) {
        AABB childBounds = GetChildBounds(node->bounds, i);
        node->children[i] = std::make_unique<QuadTreeNode>(childBounds);
    }
}

AABB FrustumQuadTree::GetChildBounds(const AABB& parentBounds, int quadrant) const {
    glm::vec3 center = parentBounds.GetCenter();
    
    glm::vec3 min, max;
    
    // 四叉树只在 XZ 平面分割，Y 轴保持不变
    switch (quadrant) {
        case 0: // 左前
            min = glm::vec3(parentBounds.min.x, parentBounds.min.y, parentBounds.min.z);
            max = glm::vec3(center.x, parentBounds.max.y, center.z);
            break;
        case 1: // 右前
            min = glm::vec3(center.x, parentBounds.min.y, parentBounds.min.z);
            max = glm::vec3(parentBounds.max.x, parentBounds.max.y, center.z);
            break;
        case 2: // 左后
            min = glm::vec3(parentBounds.min.x, parentBounds.min.y, center.z);
            max = glm::vec3(center.x, parentBounds.max.y, parentBounds.max.z);
            break;
        case 3: // 右后
            min = glm::vec3(center.x, parentBounds.min.y, center.z);
            max = glm::vec3(parentBounds.max.x, parentBounds.max.y, parentBounds.max.z);
            break;
        default:
            min = parentBounds.min;
            max = parentBounds.max;
            break;
    }
    
    return AABB(min, max);
}

int FrustumQuadTree::GetQuadrant(const AABB& nodeBounds, const AABB& entityBounds) const {
    glm::vec3 nodeCenter = nodeBounds.GetCenter();
    glm::vec3 entityCenter = entityBounds.GetCenter();
    
    bool inLeft = entityCenter.x < nodeCenter.x;
    bool inFront = entityCenter.z < nodeCenter.z;
    
    if (inLeft) {
        return inFront ? 0 : 2;
    } else {
        return inFront ? 1 : 3;
    }
}

void FrustumQuadTree::CollectNodeBounds(std::vector<AABB>& nodeBounds, size_t maxDepth) const {
    nodeBounds.clear();
    if (m_Root) {
        CollectNodeBoundsRecursive(m_Root.get(), nodeBounds, 0, maxDepth);
    }
}

void FrustumQuadTree::CollectNodeBoundsRecursive(const QuadTreeNode* node, std::vector<AABB>& nodeBounds,
                                                   size_t currentDepth, size_t maxDepth) const {
    if (!node || currentDepth > maxDepth) {
        return;
    }
    
    nodeBounds.push_back(node->bounds);
    
    if (!node->isLeaf) {
        for (int i = 0; i < 4; ++i) {
            if (node->children[i]) {
                CollectNodeBoundsRecursive(node->children[i].get(), nodeBounds, currentDepth + 1, maxDepth);
            }
        }
    }
}

AABB ComputeSceneBounds(const std::vector<AABB>& aabbs) {
    if (aabbs.empty()) {
        return AABB();
    }
    
    AABB sceneBounds;
    for (const auto& aabb : aabbs) {
        if (aabb.IsValid()) {
            sceneBounds.Expand(aabb.min);
            sceneBounds.Expand(aabb.max);
        }
    }
    
    return sceneBounds;
}

void CullingContext::Initialize(const std::vector<ECS::Entity>& allEntities) {
    m_Entities = allEntities;
    m_WorldAABBs.clear();
    m_WorldAABBs.reserve(allEntities.size());
}

void CullingContext::BuildQuadTree(const std::vector<ECS::Entity>& allEntities) {
    Initialize(allEntities);
    
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto* sceneRenderer = &g_SceneRenderer;
    
    // 预分配内存
    m_WorldAABBs.reserve(allEntities.size());
    
    for (const auto& entity : allEntities) {
        if (!coordinator.HasComponent<ECS::MeshComponent>(entity) ||
            !coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            m_WorldAABBs.push_back(AABB());
            continue;
        }
        
        auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
        auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
        
        auto* renderer = sceneRenderer->GetModelRenderer(mesh.modelPath);
        if (!renderer) {
            m_WorldAABBs.push_back(AABB());
            continue;
        }
        if (!renderer->HasModelLoaded()) {
            m_WorldAABBs.push_back(AABB());
            continue;
        }
        
        AABB localAABB = renderer->GetAABB();
        glm::mat4 modelMatrix = ECS::SceneECS::GetInstance().GetWorldMatrix(entity);  // 父链累加, 父子联动
        AABB worldAABB = localAABB.Transform(modelMatrix);
        
        m_WorldAABBs.push_back(worldAABB);
    }
    
    AABB sceneBounds = ComputeSceneBounds(m_WorldAABBs);
    
    if (!sceneBounds.IsValid()) {
        sceneBounds = AABB(glm::vec3(-100.0f), glm::vec3(100.0f));
    }
    
    glm::vec3 padding(10.0f);
    sceneBounds.min -= padding;
    sceneBounds.max += padding;
    
    m_QuadTree = FrustumQuadTree(sceneBounds, &m_WorldAABBs, 6, 8);
    
    for (size_t i = 0; i < m_WorldAABBs.size(); ++i) {
        if (m_WorldAABBs[i].IsValid()) {
            m_QuadTree.Insert(i);
        }
    }
}

void CullingContext::BuildQuadTreeAroundCamera(const std::vector<ECS::Entity>& allEntities, const glm::vec3& cameraPos, ::SceneRenderer* sceneRenderer) {
    Initialize(allEntities);

    auto& coordinator = ECS::Coordinator::GetInstance();

    size_t validAABBCount = 0;
    for (const auto& entity : allEntities) {
        if (!coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            m_WorldAABBs.push_back(AABB());
            continue;
        }

        auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
        glm::mat4 modelMatrix = transform.GetModelMatrix();
        AABB worldAABB;

        // 处理普通模型
        if (coordinator.HasComponent<ECS::MeshComponent>(entity)) {
            auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            auto* renderer = sceneRenderer->GetModelRenderer(mesh.modelPath);
            if (!renderer || !renderer->HasModelLoaded()) {
                m_WorldAABBs.push_back(AABB());
                continue;
            }
            
            AABB localAABB = renderer->GetAABB();
            worldAABB = localAABB.Transform(modelMatrix);
        }
        // 处理vox模型
        else if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
            auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
            auto* voxRenderer = sceneRenderer->GetVoxRenderer(vox.voxPath);
            if (!voxRenderer || !voxRenderer->HasLoaded()) {
                m_WorldAABBs.push_back(AABB());
                continue;
            }
            
            // 获取vox的局部AABB
            AABB localAABB;
            localAABB.min = voxRenderer->GetMinBounds();
            localAABB.max = voxRenderer->GetMaxBounds();
            worldAABB = localAABB.Transform(modelMatrix);
        }
        else {
            m_WorldAABBs.push_back(AABB());
            continue;
        }

        m_WorldAABBs.push_back(worldAABB);
        if (worldAABB.IsValid()) validAABBCount++;
    }

    // 以摄像机为中心构建四叉树边界
    // 使用固定大小的区域，确保覆盖摄像机周围的物体
    const float quadTreeRadius = 200.0f; // 四叉树半径
    glm::vec3 halfExtent(quadTreeRadius);

    AABB cameraBounds;
    cameraBounds.min = cameraPos - halfExtent;
    cameraBounds.max = cameraPos + halfExtent;

    // 不再与场景边界合并，让四叉树根节点跟随相机移动
    // 只在需要时扩展边界以包含所有物体
    AABB sceneBounds = ComputeSceneBounds(m_WorldAABBs);
    if (sceneBounds.IsValid()) {
        // 只扩展边界以包含场景中的物体，但保持以相机为中心
        // 这样四叉树根节点会跟随相机移动
        glm::vec3 cameraCenter = cameraPos;
        float maxDistFromCamera = 0.0f;
        
        for (const auto& aabb : m_WorldAABBs) {
            if (!aabb.IsValid()) continue;
            glm::vec3 entityCenter = aabb.GetCenter();
            float dist = glm::distance(cameraCenter, entityCenter);
            if (dist > maxDistFromCamera) {
                maxDistFromCamera = dist;
            }
        }
        
        // 如果有物体超出当前边界，扩展边界
        if (maxDistFromCamera > quadTreeRadius) {
            float newRadius = maxDistFromCamera + 50.0f; // 添加一些余量
            cameraBounds.min = cameraPos - glm::vec3(newRadius);
            cameraBounds.max = cameraPos + glm::vec3(newRadius);
        }
    }

    m_QuadTree = FrustumQuadTree(cameraBounds, &m_WorldAABBs, 6, 1);

    for (size_t i = 0; i < m_WorldAABBs.size(); ++i) {
        if (m_WorldAABBs[i].IsValid()) {
            m_QuadTree.Insert(i);
        }
    }
}

}
