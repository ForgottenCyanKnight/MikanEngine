#pragma once
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <glm/glm.hpp>
#include "VoxRenderer.h"
#include "VulkanManager.h"

class VoxelMeshMultiDrawIndirect {
public:
    VoxelMeshMultiDrawIndirect();
    ~VoxelMeshMultiDrawIndirect();

    // 初始化
    bool Initialize(size_t maxVoxelModels, size_t maxTotalVertices, size_t maxTotalIndices);

    // 添加体素模型
    void AddVoxelModel(void* entityId, const std::string& voxPath, const VoxRenderer* renderer, const glm::mat4& transform, const glm::vec4& color = glm::vec4(1.0f));
    void UpdateVoxelModel(void* entityId, const std::string& voxPath, const VoxRenderer* renderer, const glm::mat4& transform, const glm::vec4& color, bool visible);  // 新增：每帧更新模型数据（设置脏标记 + 可见性）

    // 渲染所有体素模型（支持背面剔除；depthOnly=true 时用 z-prepass depth 管线只写深度）
    void Render(VkCommandBuffer commandBuffer, int width, int height,
               const glm::mat4& projView, const glm::mat4& prevProjView,
               const glm::mat4& cullProjView,  // 游戏相机视锥矩阵
               const glm::vec3& cameraPosition,
               bool useDualFrustumCulling = false,
               bool enableBackfaceCulling = true,  // 是否启用背面剔除
               bool depthOnly = false);

    // 清除所有体素模型
    void Clear();
    
    // 获取总实例数量
    size_t GetTotalInstances() const { return m_totalInstances; }

private:
    // 实例数据
    struct InstanceData {
        glm::mat4 model;
        glm::mat4 prevModel;
        glm::vec4 albedoColor;
        glm::vec4 materialData;
        glm::vec3 worldMinBounds;
        float voxelSize;
    };

    // 体素模型数据
    struct VoxelModelData {
        void* entityId;  // 使用 Entity 指针作为唯一标识
        std::string voxPath;  // 缓存 voxPath 用于调试
        glm::mat4 transform;
        glm::vec4 color;
        size_t firstVertex;
        size_t vertexCount;
        size_t firstIndex;
        size_t indexCount;
        size_t firstInstance;
        size_t instanceCount;
        
        // 脏标记：标记哪些数据需要更新
        bool transformDirty;
        bool colorDirty;
        bool staticDirty;
        
        // 可见性标记：标记模型是否在视锥体内
        bool visible;
        
        // 缓存的实例数据（用于比较变化）
        InstanceData cachedInstanceData;
        
        // 每个面方向的绘制命令
        struct FaceCommand {
            size_t firstIndex;
            size_t indexCount;
            uint32_t faceDirection;
        };
        std::array<FaceCommand, 6> faceCommands;  // 6 个方向的面
    };

    // 按 VoxRenderer 分组的模型数据
    struct RendererGroup {
        std::string voxPath;  // 使用 voxPath 作为唯一标识
        const VoxRenderer* renderer;  // 缓存 VoxRenderer 指针，用于获取网格数据
        std::vector<VoxelModelData> models;
        size_t totalInstances;
    };

    // 面绘制命令结构（与 GPU 着色器一致，28字节）
    struct FaceDrawCommand {
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t vertexOffset;
        uint32_t firstInstance;
        uint32_t faceDirection;    // 面方向 0-5
        uint32_t enabled;          // 是否启用
        uint32_t padding;          // 填充到 32 字节对齐（与 GPU 端一致）
    };

    // 相机数据（用于计算着色器）
    struct CullingCameraData {
        uint32_t totalFaceCommands;      // 总面命令数（= 模型数 × 6）
        uint32_t enableHiZCulling;       // 是否启用 Hi-Z 遮挡剔除
        uint32_t screenWidth;            // 屏幕宽度
        uint32_t screenHeight;           // 屏幕高度
        glm::vec4 frustumPlanes[6];      // 视锥体平面 (Ax + By + Cz + D = 0)
        glm::vec4 viewPos;               // 相机位置（齐次坐标）
        glm::mat4 viewProj;              // 视图投影矩阵（用于 Hi-Z 遮挡测试）
        glm::mat4 prevViewProj;          // 上一帧的视图投影矩阵
    };

    // 检查设备是否支持 MDI
    bool CheckMultiDrawIndirectSupport();

    // 检查设备是否支持计算着色器
    bool CheckComputeShaderSupport();

    // 创建缓冲区
    bool CreateBuffers(size_t maxVoxelModels, size_t maxTotalVertices, size_t maxTotalIndices);

    // 创建 GPU 剔除资源
    bool CreateGPUCullingResources();

    // 创建计算着色器管线
    bool CreateCullingPipeline();

    // 创建描述符集
    bool CreateCullingDescriptorSet();

    // 合并几何数据
    void MergeGeometryData();

    // 更新实例数据和绘制命令
    void UpdateDrawCommandsAndInstanceData(const glm::vec3& cameraPosition, bool enableBackfaceCulling);

    // 执行 GPU 剔除
    void ExecuteGPUCulling(VkCommandBuffer commandBuffer,
                          const glm::mat4& projView,
                          const glm::mat4& prevProjView,
                          const glm::mat4& cullProjView,  // 第二视锥矩阵（游戏相机）
                          const glm::vec3& cameraPosition,
                          bool useDualFrustumCulling = false,
                          bool enableBackfaceCulling = true,
                          bool enableHiZCulling = true);

    // 设备是否支持 MDI
    bool m_supportsMDI;

    // 设备是否支持计算着色器
    bool m_supportsComputeShader;

    // 体素模型列表（按 VoxRenderer 分组）
    std::vector<RendererGroup> m_rendererGroups;

    // 全局顶点缓冲区
    VkBuffer m_vertexBuffer;
    VkDeviceMemory m_vertexBufferMemory;

    // 全局索引缓冲区
    VkBuffer m_indexBuffer;
    VkDeviceMemory m_indexBufferMemory;

    // 实例数据缓冲区（双缓冲）
    VkBuffer m_instanceBuffers[2];
    VkDeviceMemory m_instanceBufferMemories[2];
    void* m_mappedInstancePtrs[2];
    size_t m_currentInstanceBufferIndex;

    // 绘制命令缓冲区（双缓冲）
    VkBuffer m_drawCommandBuffers[2];
    VkDeviceMemory m_drawCommandBufferMemories[2];
    void* m_mappedDrawCommandPtrs[2];
    size_t m_currentDrawCommandBufferIndex;

    // GPU 剔除相关缓冲区
    VkBuffer m_visibleDrawCommandBuffer;
    VkDeviceMemory m_visibleDrawCommandBufferMemory;
    
    VkBuffer m_counterBuffer;
    VkDeviceMemory m_counterBufferMemory;
    uint32_t* m_mappedCounterPtr;
    
    VkBuffer m_cullingCameraBuffer;
    VkDeviceMemory m_cullingCameraBufferMemory;
    CullingCameraData* m_mappedCameraPtr;

    // 计算着色器管线
    VkPipeline m_cullingPipeline;
    VkPipelineLayout m_cullingPipelineLayout;
    VkDescriptorSetLayout m_cullingDescriptorSetLayout;
    VkDescriptorPool m_cullingDescriptorPool;
    VkDescriptorSet m_cullingDescriptorSet;

    // Dummy 图像视图（用于 Hi-Z 不可用时的描述符集更新）
    VkImage m_dummyHiZImage;
    VkDeviceMemory m_dummyHiZImageMemory;
    VkImageView m_dummyHiZImageView;

    // 最大体素模型数量
    size_t m_maxVoxelModels;

    // 最大顶点和索引数量
    size_t m_maxTotalVertices;
    size_t m_maxTotalIndices;

    // 总顶点和索引数量
    size_t m_totalVertices;
    size_t m_totalIndices;
    size_t m_totalInstances;

    // 当前面绘制命令数量（CPU 剔除后可见面数量）
    uint32_t m_currentFaceCommandCount;

    // 标记是否需要更新几何数据
    bool m_geometryDataDirty;
    
    // 命令缓冲区池
    VkCommandBuffer m_copyCommandBufferPool[4];
    size_t m_currentCommandBufferIndex;
    
    // 同步对象
    VkFence m_copyFences[4];
    size_t m_currentFenceIndex;
    
    // 预分配的临时缓冲区
    std::vector<VoxelMeshVertex> m_tempVertices;
    std::vector<uint32_t> m_tempIndices;
    
    // 网格数据缓存，用于避免相同网格重复复制
    struct MeshCacheEntry {
        size_t firstVertex;
        size_t vertexCount;
        size_t firstIndex;
        size_t indexCount;
    };
    std::unordered_map<const VoxRenderer*, MeshCacheEntry> m_meshCache;
    
    // 可见模型列表（每帧更新，只包含视锥体内的模型）
    std::vector<VoxelModelData*> m_visibleModels;
};
