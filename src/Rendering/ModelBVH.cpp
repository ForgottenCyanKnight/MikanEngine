#include "ModelBVH.h"
#include "ModelLoader.h"
#include "AABB.h"

// 三角形射线相交测试
bool intersectRayTriangle(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, 
                         const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, 
                         float& t) {
    const float EPSILON = 0.0000001f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(rayDirection, edge2);
    float a = glm::dot(edge1, h);
    
    if (a > -EPSILON && a < EPSILON) {
        return false; // 射线与三角形平行
    }
    
    float f = 1.0f / a;
    glm::vec3 s = rayOrigin - v0;
    float u = f * glm::dot(s, h);
    
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    
    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(rayDirection, q);
    
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    
    t = f * glm::dot(edge2, q);
    
    return t > EPSILON;
}

void ModelBVH::BuildFromSubMesh(const std::vector<Vertex>& vertexArray, const std::vector<uint32_t>& indexArray) {
    if (vertexArray.empty() || indexArray.empty()) {
        rootIndex = -1;
        return;
    }
    
    std::vector<BVHVertex> bvhVertices;
    bvhVertices.reserve(vertexArray.size());
    
    for (const auto& v : vertexArray) {
        // 解压顶点（压缩格式 → BVH float3）：位置直通、法线 SNORM 解码、UV half 解码
        bvhVertices.emplace_back(v.Position, UnpackSnorm3(v.Normal), UnpackHalf2(v.TexCoords));
    }
    
    std::vector<BVHVertex> deduplicatedVertices;
    std::vector<uint32_t> remappedIndices;
    std::unordered_map<BVHVertex, uint32_t, BVHVertexHash> vertexMap;
    
    deduplicatedVertices.reserve(vertexArray.size());
    remappedIndices.reserve(indexArray.size());
    
    for (uint32_t idx : indexArray) {
        const BVHVertex& v = bvhVertices[idx];
        auto it = vertexMap.find(v);
        if (it == vertexMap.end()) {
            uint32_t newIndex = static_cast<uint32_t>(deduplicatedVertices.size());
            vertexMap[v] = newIndex;
            deduplicatedVertices.push_back(v);
            remappedIndices.push_back(newIndex);
        } else {
            remappedIndices.push_back(it->second);
        }
    }

    std::cout << "Vertex deduplication: " << vertexArray.size() << " -> " << deduplicatedVertices.size() << std::endl;

    build(deduplicatedVertices, remappedIndices);
}

bool ModelBVH::IsVisibleFromPoint(const glm::vec3& point, const glm::mat4& modelMatrix) const {
    if (!IsValid()) {
        return true; // BVH无效时默认可见
    }

    glm::mat4 invModelMatrix = glm::inverse(modelMatrix);
    return IsVisibleFromPointRecursive(rootIndex, point, modelMatrix, invModelMatrix, 0);
}

bool ModelBVH::IsVisibleFromPointRecursive(int nodeIndex, const glm::vec3& point, const glm::mat4& modelMatrix, const glm::mat4& invModelMatrix, int currentDepth) const {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size())) {
        return false;
    }

    const ModelBVHNode& node = nodes[nodeIndex];

    // 有限深度控制：限制BVH遍历深度以提高性能
    const int MAX_TRAVERSAL_DEPTH = 4;
    if (currentDepth > MAX_TRAVERSAL_DEPTH) {
        return true; // 超过深度限制，默认可见
    }

    // 转换节点AABB到世界空间
    AABB localAABB(node.boundsMin, node.boundsMax);
    AABB worldAABB = localAABB.Transform(modelMatrix);

    // 检查点到节点AABB的距离
    glm::vec3 center = worldAABB.GetCenter();
    glm::vec3 rayDir = glm::normalize(center - point);
    float t;
    
    // 如果射线不与节点AABB相交，该节点不可见
    if (!AABBUtils::RayIntersectsAABB(point, rayDir, worldAABB, t)) {
        return false;
    }

    if (t <= 0.0f) {
        return true; // 点在节点内部，默认可见
    }

    // 叶子节点或达到深度限制，认为可见
    if (node.isLeaf || currentDepth >= MAX_TRAVERSAL_DEPTH) {
        return true;
    }

    // 内部节点，递归检查子节点
    bool leftVisible = IsVisibleFromPointRecursive(node.left, point, modelMatrix, invModelMatrix, currentDepth + 1);
    bool rightVisible = IsVisibleFromPointRecursive(node.right, point, modelMatrix, invModelMatrix, currentDepth + 1);

    return leftVisible || rightVisible;
}

bool ModelBVH::IsPointVisibleThroughNode(int nodeIndex, const glm::vec3& point, const glm::mat4& modelMatrix, const glm::mat4& invModelMatrix) const {
    // 简化实现：使用IsVisibleFromPointRecursive即可
    return IsVisibleFromPointRecursive(nodeIndex, point, modelMatrix, invModelMatrix, 0);
}

