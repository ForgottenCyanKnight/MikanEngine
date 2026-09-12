#pragma once
#include "Platform/Export.h"

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <array>
#include "AABB.h"
#include "ECS/Types.h"

// 前向声明
class SceneRenderer;
struct RenderWorld;

namespace Culling {

struct MIKAN_API QuadTreeNode {
    AABB bounds;
    std::vector<size_t> entityIndices;
    std::array<std::unique_ptr<QuadTreeNode>, 4> children;
    bool isLeaf = true;
    
    QuadTreeNode(const AABB& b) : bounds(b) {}
};

class MIKAN_API FrustumQuadTree {
public:
    FrustumQuadTree() : m_MaxDepth(6), m_MaxEntitiesPerNode(1), m_AllAABBs(nullptr) {}
    FrustumQuadTree(const AABB& rootBounds, const std::vector<AABB>* allAABBs, 
                    size_t maxDepth = 6, size_t maxEntitiesPerNode = 1);
    
    void Insert(size_t entityIndex);
    void Clear();
    
    // 可视化方法：收集所有节点的AABB
    void CollectNodeBounds(std::vector<AABB>& nodeBounds, size_t maxDepth = 2) const;
    
    // 获取最大深度
    size_t GetMaxDepth() const { return m_MaxDepth; }
    
private:
    std::unique_ptr<QuadTreeNode> m_Root;
    size_t m_MaxDepth;
    size_t m_MaxEntitiesPerNode;
    const std::vector<AABB>* m_AllAABBs;
    
    void InsertRecursive(QuadTreeNode* node, size_t entityIndex, size_t depth);
    void Subdivide(QuadTreeNode* node);
    void CollectNodeBoundsRecursive(const QuadTreeNode* node, std::vector<AABB>& nodeBounds, 
                                     size_t currentDepth, size_t maxDepth) const;
    int GetQuadrant(const AABB& nodeBounds, const AABB& entityBounds) const;
    AABB GetChildBounds(const AABB& parentBounds, int quadrant) const;
};

class MIKAN_API CullingContext {
public:
    void Initialize(const std::vector<ECS::Entity>& allEntities);
    void BuildQuadTree(const std::vector<ECS::Entity>& allEntities);
    void BuildQuadTreeAroundCamera(const std::vector<ECS::Entity>& allEntities, const glm::vec3& cameraPos, ::SceneRenderer* sceneRenderer);
    void BuildQuadTreeAroundCamera(const RenderWorld& world, const glm::vec3& cameraPos, ::SceneRenderer* sceneRenderer);
    
    FrustumQuadTree& GetQuadTree() { return m_QuadTree; }
    const FrustumQuadTree& GetQuadTree() const { return m_QuadTree; }
    
    const std::vector<AABB>& GetWorldAABBs() const { return m_WorldAABBs; }
    
private:
    FrustumQuadTree m_QuadTree;
    std::vector<AABB> m_WorldAABBs;
    std::vector<ECS::Entity> m_Entities;
};

AABB ComputeSceneBounds(const std::vector<AABB>& aabbs);

}
