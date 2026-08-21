#pragma once
#ifndef VOX_RENDERER_H
#define VOX_RENDERER_H

#include "RendererBase.h"
#include "VoxLoader.h"
#include "VulkanManager.h"
#include "ModelBVH.h"
#include "Rendering/VoxelTexture3DCache.h"
#include "Rendering/VoxelTexture3DManager.h"
#include <vector>
#include <unordered_map>
#include <map>
#include <set>
#include <mutex>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

struct VoxelUniformData {
    glm::mat4 projView;
    glm::mat4 prevProjView;
    glm::mat4 model;
    glm::mat4 prevModel;
    glm::vec3 cameraPosition;
    float padding;
};

struct VoxelMeshUniformData {
    glm::mat4 projView;
    glm::mat4 prevProjView;
    glm::vec3 cameraPosition;
    float padding;
};

#pragma pack(push, 1)
struct VoxelFaceData {
    glm::vec3 position;
    uint32_t data;
    glm::vec2 size;
};
#pragma pack(pop)

static_assert(sizeof(VoxelFaceData) == 24, "VoxelFaceData size mismatch! Expected 24 bytes (12 + 4 + 8)");

struct VoxelMeshVertex {
    unsigned char x, y, z;           // 3 字节 - 位置
    unsigned char r, g, b;           // 3 字节 - 颜色 RGB888
    uint16_t faceDirAndMaterial;     // 2 字节 - 面方向 + 材质索引
    // 总计：8 字节（完美对齐，无浪费）
};

struct VoxelMeshData {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    
    // 按面方向分组的索引范围
    struct FaceGroup {
        size_t firstIndex = 0;
        size_t indexCount = 0;
        uint32_t faceDirection = 0;
        glm::vec3 faceCenter;  // 该方向所有面的平均中心位置（局部空间）
        glm::vec3 faceExtent;  // 该方向面的包围盒范围（用于动态调整阈值）
    };
    
    std::array<FaceGroup, 6> faceGroups;  // 6 个方向的面
    
    size_t vertexCount = 0;
    size_t indexCount = 0;
};

struct VoxelInstanceData {
    glm::mat4 model;
    glm::mat4 prevModel;
    glm::vec4 albedoColor;
    glm::vec4 materialData;
    glm::vec3 worldMinBounds;
    float voxelSize;
};

struct VoxelFaceInstanceData {
    VoxelFaceData faceData;
    VoxelInstanceData instanceData;
};

struct VoxelRenderData {
    VkBuffer quadVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory quadVertexBufferMemory = VK_NULL_HANDLE;
    
    VkBuffer faceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory faceBufferMemory = VK_NULL_HANDLE;
    
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory instanceBufferMemory = VK_NULL_HANDLE;
    void* mappedInstancePtr = nullptr;
    
    VulkanPipeline pipeline;
    VulkanPipeline wireframePipeline;
    VulkanPipeline meshPipeline;
    VulkanPipeline meshWireframePipeline;
    VulkanPipeline depthPipeline;        // z-prepass depth-only（面片体素：voxel.vert + zprepass.frag）
    VulkanPipeline meshDepthPipeline;   // z-prepass depth-only（mesh 体素：voxel_mesh.vert + zprepass.frag）
    
    size_t faceCount = 0;
    size_t maxFaceCount = 0;
    size_t instanceCount = 0;
    size_t maxInstanceCount = 0;
    
    static constexpr size_t MAX_FRAMES_IN_FLIGHT = 2;
    VkBuffer meshInstanceBuffers[MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory meshInstanceBufferMemories[MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* meshInstanceBufferMapped[MAX_FRAMES_IN_FLIGHT] = {nullptr, nullptr};
    size_t currentMeshInstanceBufferSize = 0;
};

class VoxRenderer : public BaseRenderer {
public:
    VoxRenderer();
    virtual ~VoxRenderer();

    virtual void Init(VkRenderPass renderPass) override;
    virtual void Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) override;
    virtual void Cleanup() override;

    bool LoadVoxFile(const std::string& path, float voxelSize = 1.0f);
    bool LoadFromVoxData(const VoxFormat::VoxData& voxData, float voxelSize = 1.0f);
    
    void RenderInstanced(VkCommandBuffer commandBuffer, int width, int height, 
                         const glm::mat4& projView, const glm::mat4& prevProjView,
                         const glm::vec3& cameraPosition,
                         const std::vector<VoxelInstanceData>& instances,
                         bool depthOnly = false);
    
    void RenderWireframe(VkCommandBuffer commandBuffer, int width, int height,
                         const glm::mat4& projView, const glm::mat4& prevProjView,
                         const glm::vec3& cameraPosition,
                         const std::vector<VoxelInstanceData>& instances);
    
    void RenderMesh(VkCommandBuffer commandBuffer, int width, int height,
                    const glm::mat4& projView, const glm::mat4& prevProjView,
                    const glm::vec3& cameraPosition,
                    const std::vector<VoxelInstanceData>& instances);
    
    void RenderMeshWireframe(VkCommandBuffer commandBuffer, int width, int height,
                             const glm::mat4& projView, const glm::mat4& prevProjView,
                             const glm::vec3& cameraPosition,
                             const std::vector<VoxelInstanceData>& instances);

    // 带背面剔除的网格渲染（只绘制可见面）
    void RenderMeshWithBackfaceCulling(VkCommandBuffer commandBuffer, int width, int height,
                                        const glm::mat4& projView, const glm::mat4& prevProjView,
                                        const glm::vec3& cameraPosition,
                                        const std::vector<VoxelInstanceData>& instances,
                                        bool enableBackfaceCulling = true,
                                        bool depthOnly = false);

    bool HasLoaded() const { return m_Loaded; }    
    size_t GetVoxelCount() const { return m_VoxelCount; }
    size_t GetFaceCount() const { return m_Faces.size(); }
    glm::vec3 GetMinBounds() const { return m_MinBounds; }
    glm::vec3 GetMaxBounds() const { return m_MaxBounds; }
    glm::vec3 GetCenter() const { return (m_MinBounds + m_MaxBounds) * 0.5f; }
    float GetVoxelSize() const { return m_VoxelSize; }
    const VoxelMeshData& GetMeshData() const { return m_MeshData; }
    
    // BVH 相关方法
    void BuildBVHFromMesh(const std::vector<VoxelMeshVertex>& meshVertices, 
                          const std::vector<uint32_t>& meshIndices);
    bool LoadBVHCache();
    bool SaveBVHCache();
    std::string GetBVHCachePath() const;
    const ModelBVH& GetBVH() const { return m_BVH; }
    bool HasBVH() const { return m_BVH.IsValid(); }
    
    // Texture3D 缓存相关方法
    bool GenerateTexture3DCache();
    bool LoadTexture3DCache();
    std::string GetTexture3DCachePath() const;
    bool HasTexture3DCache() const { return !m_Texture3DData.empty(); }
    const std::vector<uint8_t>& GetTexture3DData() const { return m_Texture3DData; }
    const std::vector<VoxFormat::Color>& GetTexture3DPalette() const { return m_Texture3DPalette; }
    uint32_t GetTexture3DSizeX() const { return m_Texture3DSizeX; }
    uint32_t GetTexture3DSizeY() const { return m_Texture3DSizeY; }
    uint32_t GetTexture3DSizeZ() const { return m_Texture3DSizeZ; }
    
    // Texture3D 管理器（用于计算着色器）
    VoxelTexture3DManager& GetTexture3DManager() { return m_Texture3DManager; }
    uint32_t GetVoxelTextureIndex() const { return m_VoxelTextureIndex; }
    VkDescriptorSet GetVoxelDescriptorSet() const;
    VkPipelineLayout GetVoxelPipelineLayout() const;
    VkSampler GetVoxelSampler() const;
    
    virtual VkPipeline GetPipeline() const override { return m_RenderData.pipeline.GetPipeline(); }
    virtual VkPipelineLayout GetPipelineLayout() const override { return m_RenderData.pipeline.GetLayout(); }
    
    // 用于 MDI 渲染
    VkPipeline GetMeshPipeline() const { return m_RenderData.meshPipeline.GetPipeline(); }
    VkPipelineLayout GetMeshPipelineLayout() const { return m_RenderData.meshPipeline.GetLayout(); }
    VkPipeline GetMeshDepthPipeline() const { return m_RenderData.meshDepthPipeline.GetPipeline(); }
    VkPipelineLayout GetMeshDepthPipelineLayout() const { return m_RenderData.meshDepthPipeline.GetLayout(); }
    VkBuffer GetMeshInstanceBuffer(uint32_t frameIndex) const { return m_RenderData.meshInstanceBuffers[frameIndex]; }
    uint32_t GetCurrentFrameIndex() const { return ::GetCurrentFrameIndex(); }
    void UpdateMeshInstanceBuffer(const std::vector<VoxelInstanceData>& instances);

protected:
    void BuildVoxelFaces(const VoxFormat::VoxData& voxData, float voxelSize);
    void BuildGreedyMeshForFace(const VoxFormat::VoxData& voxData, float voxelSize, int faceDir, 
                                float offsetX, float offsetY, float offsetZ);
    void BuildTriangleMesh();
    
    // 优化的体素访问（O(1) 数组访问）
    int HasVoxelAt(int x, int y, int z) const;
    
    // 计算体素数据哈希（用于缓存查找）
    size_t ComputeVoxelDataHash(const VoxFormat::VoxData& voxData) const;
    
    // 网格缓存管理
    bool TryLoadMeshFromCache();
    void SaveMeshToCache();
    
    virtual void CreatePipeline(VkRenderPass renderPass);
    void CreateQuadVertexBuffer();
    void CreateFaceBuffer(size_t maxFaces);
    void CreateInstanceBuffer(size_t maxInstances);
    void CreateMeshInstanceBuffer(size_t maxInstances);
    void CreateMeshBuffers(const std::vector<VoxelMeshVertex>& vertices, const std::vector<uint32_t>& indices);
    void UpdateInstanceBuffer(const std::vector<VoxelInstanceData>& instances);
    void UpdateFaceInstanceBuffer(const std::vector<VoxelFaceInstanceData>& faceInstanceData);
    void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    
    VoxelRenderData m_RenderData;

private:
    VoxelMeshData m_MeshData;
    std::vector<VoxelFaceData> m_Faces;
    
    // 体素空间占用表（使用线性数组替代 unordered_map，O(1) 访问）
    // 对于 256³ 的体素，使用 16MB 内存换取 50-100 倍性能提升
    std::vector<int8_t> m_VoxelGrid;
    glm::ivec3 m_GridSize = glm::ivec3(0);
    
    // 可见面网格缓存（基于体素数据哈希，避免重复计算）
    struct MeshCacheEntry {
        size_t hash;
        std::vector<VoxelFaceData> faces;
        VoxelMeshData meshData;
        bool isValid = false;
    };
    
    // 全局网格缓存（跨 VoxRenderer 实例共享）
    static std::unordered_map<size_t, MeshCacheEntry> s_meshCache;
    static std::mutex s_meshCacheMutex;
    size_t m_voxelDataHash = 0;  // 当前体素数据的哈希值
    bool m_useCachedMesh = false;  // 是否使用了缓存的网格
    
    std::string m_FilePath;
    float m_VoxelSize = 1.0f;
    bool m_Loaded = false;
    size_t m_VoxelCount = 0;
    
    glm::vec3 m_MinBounds = glm::vec3(0.0f);
    glm::vec3 m_MaxBounds = glm::vec3(0.0f);
    
    // BVH 数据
    ModelBVH m_BVH;
    
    // Texture3D 缓存数据
    VoxelCache::Texture3DCacheGenerator m_Texture3DCacheGen;
    std::vector<uint8_t> m_Texture3DData;
    std::vector<VoxFormat::Color> m_Texture3DPalette;
    uint32_t m_Texture3DSizeX = 0;
    uint32_t m_Texture3DSizeY = 0;
    uint32_t m_Texture3DSizeZ = 0;
    
    // Texture3D 管理器
    VoxelTexture3DManager m_Texture3DManager;
    uint32_t m_VoxelTextureIndex = UINT32_MAX;  // 当前体素模型在管理器中的索引
    
    // 背面剔除缓存
    struct BackfaceCullingCache {
        // 缓存的相机位置（用于检测相机是否移动）
        glm::vec3 cachedCameraPosition;
        glm::mat4 cachedProjMatrix;
        
        // 每个实例的可见面方向位掩码（6 个方向，每个方向 1 bit）
        // key = 实例索引，value = 可见面方向位掩码
        std::vector<uint8_t> visibleFaceMasks;
        
        // 缓存是否有效
        bool isValid = false;
        
        // 清空缓存
        void clear() {
            visibleFaceMasks.clear();
            isValid = false;
        }
        
        // 检查缓存是否可用
        bool isCacheValid(const glm::vec3& cameraPos, const glm::mat4& projMatrix) const {
            if (!isValid) return false;
            
            // 检查相机位置是否变化（使用小阈值避免浮点误差）
            const float epsilon = 0.001f;
            if (glm::distance(cameraPos, cachedCameraPosition) > epsilon) {
                return false;
            }
            
            // 检查投影矩阵是否变化
            const float matrixEpsilon = 0.001f;
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    if (glm::abs(projMatrix[i][j] - cachedProjMatrix[i][j]) > matrixEpsilon) {
                        return false;
                    }
                }
            }
            
            return true;
        }
    };
    
    BackfaceCullingCache m_cullingCache;
    
    // 更新缓存
    void updateCullingCache(const glm::vec3& cameraPos, const glm::mat4& projMatrix,
                           size_t instanceCount, const std::vector<uint8_t>& faceMasks);
    bool m_Texture3DManagerInitialized = false;
};

#endif
