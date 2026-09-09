#pragma once
#include "Platform/Export.h"

#include "RendererBase.h"
#include "AABB.h"
#include <glm/glm.hpp>
#include <vector>

// 线框实例数据
struct MIKAN_API WireframeInstance {
    glm::vec3 position;     // AABB中心位置
    glm::vec3 size;         // AABB半边长
    glm::vec4 rotation;     // 旋转四元数
    glm::vec3 color;        // 线框颜色
};

class MIKAN_API WireframeRenderer {
public:
    WireframeRenderer();
    ~WireframeRenderer();

    // 惰性初始化：首次 Render（或 render pass 重建后）调用；管线绑 UI overlay pass（链末叠加，无深度）
    void EnsureInit(VkRenderPass uiPass);
    
    // 清理资源
    void Cleanup();
    
    // 添加 AABB 线框实例
    void AddAABB(const AABB& aabb, const glm::vec3& color = glm::vec3(1.0f, 0.0f, 0.0f));
    void AddAABB(const AABB& aabb, const glm::vec3& position, const glm::vec4& rotation, const glm::vec3& color = glm::vec3(1.0f, 0.0f, 0.0f));
    
    // 添加 OBB（有向包围盒）线框实例
    // center: OBB 中心（世界空间）
    // halfExtents: 局部空间的半边长
    // rotation: 旋转四元数
    void AddOBB(const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec4& rotation, 
                const glm::vec3& color = glm::vec3(0.0f, 1.0f, 0.0f));
    
    // 添加 OBB（从模型矩阵计算）
    // localAABB: 模型局部空间的 AABB
    // modelMatrix: 模型的世界变换矩阵
    void AddOBBFromMatrix(const AABB& localAABB, const glm::mat4& modelMatrix, 
                          const glm::vec3& color = glm::vec3(0.0f, 1.0f, 0.0f));
    
    // 添加以中心点为基准的立方体线框（用于BVH可视化）
    void AddCenteredCube(const glm::vec3& center, const glm::vec3& halfSize, const glm::vec3& color = glm::vec3(1.0f, 1.0f, 1.0f));
    
    // 添加线段（用于四叉树可视化）
    void AddLine(const glm::vec3& start, const glm::vec3& end, 
                 const glm::vec3& color = glm::vec3(1.0f, 1.0f, 1.0f));
    
    // 添加视锥体线框（用于显示摄像机视野）
    void AddFrustum(const Frustum& frustum, const glm::vec3& color = glm::vec3(1.0f, 1.0f, 0.0f));
    
    // 添加视锥体线框（直接传入角点）
    void AddFrustum(const std::array<glm::vec3, 8>& corners, const glm::vec3& color = glm::vec3(1.0f, 1.0f, 0.0f));
    
    // 清除所有实例
    void ClearInstances();
    
    // 清除视锥体线框
    void ClearFrustums();
    
    // 渲染线框
    void Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj);
    
    // 渲染视锥体线框（单独渲染，使用动态顶点）
    void RenderFrustums(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj);
    
    // 检查是否有实例需要渲染
    bool HasInstances() const { return !m_Instances.empty(); }
    
    // 检查是否有视锥体需要渲染
    bool HasFrustums() const { return !m_FrustumVertices.empty(); }

private:
    // 创建立方体线框顶点数据
    void CreateCubeVertices();
    
    // 创建渲染管线
    void CreatePipeline(VkRenderPass renderPass);
    void CreateFrustumPipeline(VkRenderPass renderPass);
    
    // 更新实例缓冲区
    void UpdateInstanceBuffer();
    void UpdateFrustumBuffer();

private:
    // 顶点缓冲区
    VulkanBuffer m_VertexBuffer;
    
    // 实例缓冲区
    VulkanBuffer m_InstanceBuffer;    
    // Uniform缓冲区（用于push constant）
    struct MIKAN_API UniformBufferObject {
        glm::mat4 view;
        glm::mat4 proj;
    };
    
    // 渲染管线
    VulkanPipeline m_Pipeline;
    VulkanPipeline m_FrustumPipeline;
    
    // 实例数据
    std::vector<WireframeInstance> m_Instances;
    
    // 顶点数量
    uint32_t m_VertexCount = 0;
    
    // 是否需要更新实例缓冲区
    bool m_InstanceBufferDirty = true;
    
    // 视锥体顶点数据（每个视锥体24个顶点，12条边）
    std::vector<glm::vec3> m_FrustumVertices;
    std::vector<glm::vec3> m_FrustumColors;
    
    // 视锥体顶点缓冲区
    VulkanBuffer m_FrustumVertexBuffer;
    VulkanBuffer m_FrustumColorBuffer;
    bool m_FrustumBufferDirty = true;

    VkRenderPass m_UIPass = VK_NULL_HANDLE;
    bool m_Initialized = false;
};
