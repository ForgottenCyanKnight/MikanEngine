#pragma once
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <glm/glm.hpp>
#include "VoxRenderer.h"
#include "VulkanManager.h"
#include "Rendering/RendererBase.h"
#include <memory>

class VoxelMeshMultiDrawIndirect {
public:
    VoxelMeshMultiDrawIndirect();
    ~VoxelMeshMultiDrawIndirect();

    // 初始化
    bool Initialize(size_t maxVoxelModels, size_t maxTotalVertices, size_t maxTotalIndices);

    // 添加体素模型
    void AddVoxelModel(void* entityId, const std::string& voxPath, const VoxRenderer* renderer, const glm::mat4& transform, const glm::vec4& color = glm::vec4(1.0f));
    void UpdateVoxelModel(void* entityId, const std::string& voxPath, const VoxRenderer* renderer, const glm::mat4& transform, const glm::vec4& color, bool visible, const glm::mat4* previousModel = nullptr);  // 新增：每帧更新模型数据（设置脏标记 + 可见性）

    // 消费当前视口已准备的 GPU 间接命令与实例流。
    void Render(VkCommandBuffer commandBuffer, int width, int height,
               const glm::mat4& projView, const glm::mat4& prevProjView,
               const glm::mat4& cullProjView,  // 游戏相机视锥矩阵
               const glm::vec3& cameraPosition,
               bool useDualFrustumCulling = false,
               bool enableBackfaceCulling = true, int viewSlot = 0);

    // Record outside every render pass. Collection includes all eligible instances.
    bool BeginGpuCollection(uint64_t epoch);
    bool HasPreparedGpuResults(int viewSlot, uint64_t epoch) const;
    void PrepareGpuCull(VkCommandBuffer commandBuffer, int viewSlot, uint64_t epoch,
                        const glm::mat4& rasterProjView, const glm::mat4& mainProjView,
                        bool mainFrustum, bool sceneFrustum, const glm::vec3& rasterCamera);

    // 清除所有体素模型
    void Clear();

    // 获取总实例数量
    size_t GetTotalInstances() const { return m_totalInstances; }

private:
    struct GpuFrame {
        VulkanBuffer sources, candidates, commands, instances, params, quads, quadCommands;
        VkDescriptorSet quadDescriptor=VK_NULL_HANDLE;
        size_t quadCapacity=0;
        uint64_t quadVersion=0;
        bool useQuads=false;
        std::vector<uint8_t> residentQuads;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkBufferView sourceView = VK_NULL_HANDLE;
        std::vector<uint8_t> residentSources, residentCandidates, residentCommands;
        uint64_t epoch = UINT64_MAX;
        uint32_t drawCount = 0;
        size_t capacity = 0;
        size_t commandCapacity = 0;
    };
    std::vector<VoxQuad> m_QuadAtlas;
    std::vector<uint32_t> m_QuadAtlasWords;
    bool m_QuadAtlasValid=false;
    uint64_t m_QuadAtlasVersion=1;
    std::unordered_map<uint32_t, std::unique_ptr<GpuFrame>> m_GpuFrames;
    uint64_t m_GpuCollectionEpoch = UINT64_MAX;
    std::unordered_map<void*, std::pair<size_t, size_t>> m_ModelSlots;
    std::unordered_map<std::string, size_t> m_GroupSlots;
    VkPipeline m_IndirectCullPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_IndirectCullLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_IndirectCullSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_IndirectCullPool = VK_NULL_HANDLE;
    bool CreateIndirectCullPipeline();
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
        uint64_t geometryRevision=0;
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

    // 检查设备是否支持 MDI
    bool CheckMultiDrawIndirectSupport();

    // 检查设备是否支持计算着色器
    bool CheckComputeShaderSupport();

    void MergeGeometryData();

    // 设备是否支持 MDI
    bool m_supportsMDI;

    // 设备是否支持计算着色器
    bool m_supportsComputeShader;

    // 体素模型列表（按 VoxRenderer 分组）
    std::vector<RendererGroup> m_rendererGroups;

    // 总顶点和索引数量
    size_t m_totalVertices;
    size_t m_totalIndices;
    size_t m_totalInstances;

    bool m_geometryDataDirty;

    // 网格数据缓存，用于避免相同网格重复复制
    struct MeshCacheEntry {
        size_t firstVertex;
        size_t vertexCount;
        size_t firstIndex;
        size_t indexCount;
    };
    std::unordered_map<const VoxRenderer*, MeshCacheEntry> m_meshCache;

};
