#include "ModelBVH.h"
#include "ModelLoader.h"
#include "Core/LogStream.h"

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

    LOGSTREAM(Info) << "Vertex deduplication: " << vertexArray.size() << " -> " << deduplicatedVertices.size() << std::endl;

    build(deduplicatedVertices, remappedIndices);
}

