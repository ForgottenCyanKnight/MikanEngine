#pragma once
#include "Platform/Export.h"

#include <glm/glm.hpp>
#include <array>
#include <vector>

struct MIKAN_API Plane {
    glm::vec3 normal;
    float distance;

    bool operator==(const Plane& other) const {
        return normal == other.normal && glm::abs(distance - other.distance) < 1e-6f;
    }

    Plane() : normal(0.0f, 1.0f, 0.0f), distance(0.0f) {}
    Plane(const glm::vec4& p) {
        normal = glm::vec3(p.x, p.y, p.z);
        distance = p.w;
        float length = glm::length(normal);
        normal /= length;
        distance /= length;
    }
};

struct MIKAN_API AABB {
    glm::vec3 min;
    glm::vec3 max;

    AABB() : min(glm::vec3(FLT_MAX)), max(glm::vec3(-FLT_MAX)) {}
    AABB(const glm::vec3& minVal, const glm::vec3& maxVal) : min(minVal), max(maxVal) {}

    glm::vec3 GetCenter() const {
        return (min + max) * 0.5f;
    }

    glm::vec3 GetSize() const {
        return max - min;
    }

    float GetMaxDimension() const {
        glm::vec3 size = GetSize();
        return glm::max(size.x, glm::max(size.y, size.z));
    }

    // 联合方法
    static AABB Union(const AABB& a, const AABB& b) {
        return AABB(
            glm::min(a.min, b.min),
            glm::max(a.max, b.max)
        );
    }
    
    // 成员函数版本的联合
    AABB UnionWith(const AABB& other) const {
        return AABB(
            glm::min(min, other.min),
            glm::max(max, other.max)
        );
    }
    
    // 扩展AABB包含一个点
    void Expand(const glm::vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
    
    // 表面积
    float SurfaceArea() const {
        glm::vec3 extent = max - min;
        return 2.0f * (extent.x * extent.y + extent.x * extent.z + extent.y * extent.z);
    }
    
    // 检查是否有效
    bool IsValid() const {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    bool Intersects(const AABB& other) const {
        return (min.x <= other.max.x && max.x >= other.min.x) &&
               (min.y <= other.max.y && max.y >= other.min.y) &&
               (min.z <= other.max.z && max.z >= other.min.z);
    }

    bool Contains(const glm::vec3& point) const {
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    static AABB Merge(const AABB& a, const AABB& b) {
        return {
            glm::min(a.min, b.min),
            glm::max(a.max, b.max)
        };
    }

    bool IsInsideFrustum(const std::array<Plane, 6>& planes) const {
        for (const auto& plane : planes) {
            glm::vec3 p(
                plane.normal.x >= 0 ? max.x : min.x,
                plane.normal.y >= 0 ? max.y : min.y,
                plane.normal.z >= 0 ? max.z : min.z
            );
            float distance = glm::dot(plane.normal, p) + plane.distance;
            if (distance < 0) return false;
        }
        return true;
    }
    
    // 通过变换矩阵变换 AABB（正确处理非均匀缩放和剪切）
    AABB Transform(const glm::mat4& transform) const {
        // 从平移开始
        glm::vec3 newMin = glm::vec3(transform[3]);
        glm::vec3 newMax = newMin;
        
        // 对每个轴，计算 min 和 max 与矩阵列的乘积，找到极值
        for (int c = 0; c < 3; ++c) {
            glm::vec3 col = glm::vec3(transform[c]);
            
            float minVal = (c == 0) ? min.x : (c == 1) ? min.y : min.z;
            float maxVal = (c == 0) ? max.x : (c == 1) ? max.y : max.z;
            
            glm::vec3 a = col * minVal;
            glm::vec3 b = col * maxVal;
            
            newMin += glm::min(a, b);
            newMax += glm::max(a, b);
        }
        
        return AABB(newMin, newMax);
    }
    
    // 获取AABB的8个角点
    std::array<glm::vec3, 8> GetCorners() const {
        return {
            glm::vec3(min.x, min.y, min.z),
            glm::vec3(max.x, min.y, min.z),
            glm::vec3(min.x, max.y, min.z),
            glm::vec3(max.x, max.y, min.z),
            glm::vec3(min.x, min.y, max.z),
            glm::vec3(max.x, min.y, max.z),
            glm::vec3(min.x, max.y, max.z),
            glm::vec3(max.x, max.y, max.z)
        };
    }
};

// 视锥体结构体 - 用于表示摄像机的视野范围
struct MIKAN_API Frustum {
    std::array<Plane, 6> planes;
    
    // 视锥体的8个角点（用于线框渲染）
    std::array<glm::vec3, 8> corners;
    
    Frustum() = default;
    
    // 从视图投影矩阵构建视锥体
    static Frustum FromViewProj(const glm::mat4& viewProj, float nearDist = 0.1f, float farDist = 100.0f, float fov = 45.0f, float aspect = 16.0f/9.0f);
    
    // 获取视锥体边缘线段的顶点列表（用于线框渲染）
    // 返回12条边的顶点对（每条边2个顶点，共24个顶点）
    std::vector<glm::vec3> GetLineVertices() const;
};

namespace AABBUtils {
    inline std::array<Plane, 6> ExtractFrustumPlanes(const glm::mat4& viewProj, float expandDistance = 0.0f) {
        std::array<Plane, 6> planes;
        glm::mat4 m = glm::transpose(viewProj);

        planes[0] = Plane(m[3] + m[0]); // Left
        planes[1] = Plane(m[3] - m[0]); // Right
        planes[2] = Plane(m[3] + m[1]); // Bottom
        planes[3] = Plane(m[3] - m[1]); // Top
        planes[4] = Plane(m[3] + m[2]); // Near
        planes[5] = Plane(m[3] - m[2]); // Far

        for (auto& plane : planes) {
            plane.distance -= expandDistance;
        }

        return planes;
    }
    
    inline bool IsAABBInFrustum(const AABB& aabb, const std::array<Plane, 6>& planes) {
        for (const auto& plane : planes) {
            glm::vec3 positiveVertex = aabb.min;
            if (plane.normal.x >= 0) positiveVertex.x = aabb.max.x;
            if (plane.normal.y >= 0) positiveVertex.y = aabb.max.y;
            if (plane.normal.z >= 0) positiveVertex.z = aabb.max.z;
            float distance = glm::dot(positiveVertex, plane.normal) + plane.distance;
            if (distance < 0) return false;
        }
        return true;
    }

    inline bool RayIntersectsAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
        const AABB& aabb, float& t) {
        glm::vec3 dirfrac = 1.0f / rayDir;

        float t1 = (aabb.min.x - rayOrigin.x) * dirfrac.x;
        float t2 = (aabb.max.x - rayOrigin.x) * dirfrac.x;
        float t3 = (aabb.min.y - rayOrigin.y) * dirfrac.y;
        float t4 = (aabb.max.y - rayOrigin.y) * dirfrac.y;
        float t5 = (aabb.min.z - rayOrigin.z) * dirfrac.z;
        float t6 = (aabb.max.z - rayOrigin.z) * dirfrac.z;

        float tmin = glm::max(glm::max(glm::min(t1, t2), glm::min(t3, t4)), glm::min(t5, t6));
        float tmax = glm::min(glm::min(glm::max(t1, t2), glm::max(t3, t4)), glm::max(t5, t6));

        if (tmax < 0) {
            t = tmax;
            return false;
        }
        if (tmin > tmax) {
            t = tmax;
            return false;
        }

        t = tmin;
        return true;
    }
    
    // 计算两个AABB之间的距离
    inline float Distance(const AABB& a, const AABB& b) {
        float distance = 0.0f;
        
        // 计算x方向的距离
        if (a.max.x < b.min.x) {
            distance += (b.min.x - a.max.x) * (b.min.x - a.max.x);
        } else if (b.max.x < a.min.x) {
            distance += (a.min.x - b.max.x) * (a.min.x - b.max.x);
        }
        
        // 计算y方向的距离
        if (a.max.y < b.min.y) {
            distance += (b.min.y - a.max.y) * (b.min.y - a.max.y);
        } else if (b.max.y < a.min.y) {
            distance += (a.min.y - b.max.y) * (a.min.y - b.max.y);
        }
        
        // 计算z方向的距离
        if (a.max.z < b.min.z) {
            distance += (b.min.z - a.max.z) * (b.min.z - a.max.z);
        } else if (b.max.z < a.min.z) {
            distance += (a.min.z - b.max.z) * (a.min.z - b.max.z);
        }
        
        return glm::sqrt(distance);
    }
    
    // 合并两个AABB
    inline AABB Union(const AABB& a, const AABB& b) {
        return AABB(
            glm::min(a.min, b.min),
            glm::max(a.max, b.max)
        );
    }
}

// 视锥体结构体实现
inline Frustum Frustum::FromViewProj(const glm::mat4& viewProj, float nearDist, float farDist, float fov, float aspect) {
    Frustum frustum;
    
    // 提取视锥体平面
    frustum.planes = AABBUtils::ExtractFrustumPlanes(viewProj);
    
    // 计算视锥体的8个角点
    glm::mat4 invViewProj = glm::inverse(viewProj);
    
    // NDC空间的8个角点
    std::array<glm::vec4, 8> ndcCorners = {
        glm::vec4(-1.0f, -1.0f, -1.0f, 1.0f), // 近平面左下
        glm::vec4( 1.0f, -1.0f, -1.0f, 1.0f), // 近平面右下
        glm::vec4( 1.0f,  1.0f, -1.0f, 1.0f), // 近平面右上
        glm::vec4(-1.0f,  1.0f, -1.0f, 1.0f), // 近平面左上
        glm::vec4(-1.0f, -1.0f,  1.0f, 1.0f), // 远平面左下
        glm::vec4( 1.0f, -1.0f,  1.0f, 1.0f), // 远平面右下
        glm::vec4( 1.0f,  1.0f,  1.0f, 1.0f), // 远平面右上
        glm::vec4(-1.0f,  1.0f,  1.0f, 1.0f)  // 远平面左上
    };
    
    for (int i = 0; i < 8; i++) {
        glm::vec4 worldPos = invViewProj * ndcCorners[i];
        frustum.corners[i] = glm::vec3(worldPos) / worldPos.w;
    }
    
    return frustum;
}

inline std::vector<glm::vec3> Frustum::GetLineVertices() const {
    std::vector<glm::vec3> lines;
    lines.reserve(24);
    
    // 近平面的4条边
    lines.push_back(corners[0]); lines.push_back(corners[1]);
    lines.push_back(corners[1]); lines.push_back(corners[2]);
    lines.push_back(corners[2]); lines.push_back(corners[3]);
    lines.push_back(corners[3]); lines.push_back(corners[0]);
    
    // 远平面的4条边
    lines.push_back(corners[4]); lines.push_back(corners[5]);
    lines.push_back(corners[5]); lines.push_back(corners[6]);
    lines.push_back(corners[6]); lines.push_back(corners[7]);
    lines.push_back(corners[7]); lines.push_back(corners[4]);
    
    // 连接近平面和远平面的4条边
    lines.push_back(corners[0]); lines.push_back(corners[4]);
    lines.push_back(corners[1]); lines.push_back(corners[5]);
    lines.push_back(corners[2]); lines.push_back(corners[6]);
    lines.push_back(corners[3]); lines.push_back(corners[7]);
    
    return lines;
}
