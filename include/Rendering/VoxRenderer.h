#pragma once
#ifndef VOX_RENDERER_H
#define VOX_RENDERER_H

#include "RendererBase.h"
#include "VoxLoader.h"
#include "VoxQuad.h"
#include "VulkanManager.h"
#include "ModelBVH.h"
#include "Rendering/VoxelTexture3DCache.h"
#include "Rendering/VoxelTexture3DManager.h"
#include <vector>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <map>
#include <set>
#include <mutex>
#include <memory>
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

// Plane-local U8,V8,widthMinusOne8,heightMinusOne8; no per-quad RGB.
// Face direction is supplied by the GPU visible-instance stream.
// VoxQuad is the shared four-byte plane-local geometry defined in VoxQuad.h.
// CPU-only position scratch used to build the picking BVH. Never uploaded for drawing.
struct VoxPickingVertex { uint8_t x,y,z; };

// Index values restart every 16384 quads; vertices retain their original order.
inline constexpr size_t VOX_INDEX_SEGMENT_VERTICES = 65536;
inline constexpr size_t VOX_INDEX_SEGMENT_INDICES = 16384 * 6;
template<class Callback>
inline void ForEachVoxIndexSegment(size_t firstIndex, size_t indexCount, Callback&& callback)
{
    while (indexCount) {
        const size_t segment = firstIndex / VOX_INDEX_SEGMENT_INDICES;
        const size_t available = VOX_INDEX_SEGMENT_INDICES - firstIndex % VOX_INDEX_SEGMENT_INDICES;
        const size_t count = indexCount < available ? indexCount : available;
        callback(firstIndex, count, segment * VOX_INDEX_SEGMENT_VERTICES);
        firstIndex += count;
        indexCount -= count;
    }
}

struct VoxelMeshData {


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
    static constexpr size_t MAX_FRAMES_IN_FLIGHT = 2;
    struct MeshInstanceUpload {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        size_t capacity = 0;
    };
    // 每个 frame 可拥有多个上传段：同一 command buffer 内的反射探针六面、
    // 以及可能连续绘制的多个体素模型，不能共享一份 host-visible 实例流。
    std::array<std::vector<MeshInstanceUpload>, MAX_FRAMES_IN_FLIGHT>
        meshInstanceUploads;
    uint64_t meshInstanceUploadFrameSerial = UINT64_MAX;
    uint32_t meshInstanceUploadCursor[MAX_FRAMES_IN_FLIGHT] = {0, 0};
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
    static bool CookSurfaceFile(const std::string& input, const std::string& output, bool uniform = false);
    bool LoadCompiledSurface(const std::string& path, float voxelSize);
    bool LoadFromVoxData(const VoxFormat::VoxData& voxData, float voxelSize = 1.0f);
    bool IsComposite() const { return !m_Submeshes.empty(); }
    template<class F> void VisitSurfaces(F&& visit) const {
        if(!IsComposite()){visit(*this,glm::mat4(1));return;}
        for(const auto& instance:m_SurfaceInstances)
            visit(*m_Submeshes[instance.modelIndex],instance.transform);
    }

    void RenderInstanced(VkCommandBuffer commandBuffer, int width, int height,
                         const glm::mat4& projView, const glm::mat4& prevProjView,
                         const glm::vec3& cameraPosition,
                         const std::vector<VoxelInstanceData>& instances);

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
                                        bool enableBackfaceCulling = true);

    bool HasLoaded() const { return m_Loaded; }
    bool PrepareCsmInstances(VkRenderPass renderPass, const std::vector<VoxelInstanceData>& instances,
                             uint64_t renderEpoch);
    void RenderCsmDepth(VkCommandBuffer commandBuffer, const glm::mat4& shadowMatrix,
                        const std::vector<VoxelInstanceData>& instances,
                        const std::array<Plane, 6>& cascadePlanes);
    size_t GetVoxelCount() const { return m_VoxelCount; }
    size_t GetFaceCount() const {
        if(!IsComposite())return m_Faces.size();
        size_t count=0;for(const auto& instance:m_SurfaceInstances)count+=m_Submeshes[instance.modelIndex]->GetFaceCount();return count;
    }
    glm::vec3 GetMinBounds() const { return m_MinBounds; }
    glm::vec3 GetMaxBounds() const { return m_MaxBounds; }
    glm::vec3 GetCenter() const { return (m_MinBounds + m_MaxBounds) * 0.5f; }
    float GetVoxelSize() const { return m_VoxelSize; }
    const VoxelMeshData& GetMeshData() const { return m_MeshData; }

    // BVH 相关方法
    void BuildBVHFromMesh(const std::vector<VoxPickingVertex>& meshVertices,
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
    uint32_t GetDdaGridId() const { return m_DdaGridId; }
    uint32_t m_DdaGridId=UINT32_MAX;
    VkDescriptorSet GetVoxelDescriptorSet() const;
    VkPipelineLayout GetVoxelPipelineLayout() const;
    VkSampler GetVoxelSampler() const;

    virtual VkPipeline GetPipeline() const override { return m_QuadPipeline.GetPipeline(); }
    virtual VkPipelineLayout GetPipelineLayout() const override { return m_QuadPipeline.GetLayout(); }

    // 用于 MDI 渲染
    uint64_t GetGeometryRevision() const { return m_GeometryRevision; }
    const std::vector<VoxQuad>& GetQuads() const { return m_Quads; }
    const std::vector<VoxPlaneRange>& GetPlaneRanges() const { return m_PlaneRanges; }
    std::pair<uint32_t,uint32_t> DecodeQuad(uint32_t index) const;
    const std::vector<uint32_t>& GetQuadMaterials() const { return m_QuadMaterials; }
    void SetUniformSurfaceMaterials(bool required);
    const std::vector<uint32_t>& GetSurfaceAttributes() const { return m_SurfaceAttributes; }
    bool HasEmissiveQuads() const { return m_HasEmissiveQuads; }
    bool HasValidQuads() const { return m_QuadEncodingValid && !m_Quads.empty(); }
    static VkBuffer GetSharedQuadIndexBuffer();
    VkPipeline GetQuadPipeline() const { return m_QuadPipeline.GetPipeline(); }
    VkPipelineLayout GetQuadPipelineLayout() const { return m_QuadPipeline.GetLayout(); }
    VkDescriptorSetLayout GetQuadSetLayout() const { return m_QuadSetLayout; }
    VkBuffer GetMeshInstanceBuffer(uint32_t frameIndex) const {
        if (frameIndex >= VoxelRenderData::MAX_FRAMES_IN_FLIGHT ||
            m_RenderData.meshInstanceUploads[frameIndex].empty()) {
            return VK_NULL_HANDLE;
        }
        return m_RenderData.meshInstanceUploads[frameIndex].front().buffer;
    }
    uint32_t GetCurrentFrameIndex() const { return ::GetCurrentFrameIndex(); }
    VkBuffer UpdateMeshInstanceBuffer(const std::vector<VoxelInstanceData>& instances);

protected:
    void BuildVoxelFaces(const VoxFormat::VoxData& voxData, float voxelSize);

    void BuildTriangleMesh();

    // 优化的体素访问（O(1) 数组访问）
    int HasVoxelAt(int x, int y, int z) const;

    // 计算体素数据哈希（用于缓存查找）
    size_t ComputeVoxelDataHash(const VoxFormat::VoxData& voxData) const;

    // 网格缓存管理
    bool TryLoadMeshFromCache();
    void SaveMeshToCache();

    virtual void CreatePipeline(VkRenderPass renderPass);
    virtual bool IsPreviewPipeline() const { return m_CompositePreview; }
    void CreateMeshInstanceBuffer(size_t maxInstances);
    bool CreateMeshInstanceUpload(VoxelRenderData::MeshInstanceUpload& upload,
                                  size_t maxInstances);
    void DestroyMeshInstanceUpload(VoxelRenderData::MeshInstanceUpload& upload);

    VoxelRenderData m_RenderData;

private:
    std::vector<std::unique_ptr<VoxRenderer>> m_Submeshes;
    std::vector<VoxFormat::SceneInstance> m_SurfaceInstances;
    VkRenderPass m_CompositeRenderPass=VK_NULL_HANDLE;
    bool m_CompositePreview=false;
    void ClearComposite();
    void FinishComposite(float voxelSize);
    bool LoadComposite(const VoxFormat::VoxData&,float voxelSize);
    std::vector<VoxelInstanceData> ComponentInstances(uint32_t mesh,const std::vector<VoxelInstanceData>&) const;
    std::vector<uint32_t> EncodeCompiledSurface() const;
    bool LoadCompiledSurfaceWords(std::vector<uint32_t> words,float voxelSize);
    VoxelMeshData m_MeshData;
    std::vector<VoxelFaceData> m_Faces;
    std::vector<uint32_t> m_FaceMaterials;  // 与 m_Faces 平行，greedy 阶段按 colorIndex 记录材质

    // 体素空间占用表（使用线性数组替代 unordered_map，O(1) 访问）
    // 对于 256³ 的体素，使用 16MB 内存换取 50-100 倍性能提升
    std::vector<uint8_t> m_VoxelGrid;
    glm::ivec3 m_GridSize = glm::ivec3(0);

    // 可见面网格缓存（基于体素数据哈希，避免重复计算）
    struct MeshCacheEntry {
        size_t hash;
        std::vector<VoxelFaceData> faces;
        std::vector<uint32_t> faceMaterials;  // 与 faces 平行，MATT 材质字
        std::vector<uint32_t> surfaceAttributes;
        bool hasEmissiveQuads=false;          // 任一 quad 带 MATT 自发光位
        VoxelMeshData meshData;
        bool isValid = false;
    };

    // 全局网格缓存（跨 VoxRenderer 实例共享）
    static std::unordered_map<size_t, MeshCacheEntry> s_meshCache;
    static std::mutex s_meshCacheMutex;
    size_t m_voxelDataHash = 0;  // 当前体素数据的哈希值
    bool m_useCachedMesh = false;  // 是否使用了缓存的网格

    std::string m_FilePath;
    void RenderQuadsDirect(VkCommandBuffer commandBuffer, int width, int height,
        const glm::mat4& projView, const glm::mat4& prevProjView, const glm::vec3& cameraPosition,
        const std::vector<VoxelInstanceData>& instances, bool wireframe);
    bool m_SharedQuadIndicesAcquired=false;
    VulkanBuffer m_DirectQuads;
    VkDescriptorSetLayout m_DirectQuadLayout=VK_NULL_HANDLE;
    VkDescriptorPool m_DirectQuadPool=VK_NULL_HANDLE;
    VkDescriptorSet m_DirectQuadDescriptor=VK_NULL_HANDLE;
    VulkanPipeline m_DirectQuadPipeline, m_DirectQuadWirePipeline;
    uint64_t m_GeometryRevision=0;
    std::vector<VoxQuad> m_Quads;
    std::vector<VoxPlaneRange> m_PlaneRanges;
    bool m_UniformSurfaceMaterials=false;
    std::vector<uint32_t> m_SurfaceAttributes;
    std::vector<uint32_t> m_QuadMaterials;  // 与 m_Quads 平行，供 RT 命中着色读取
    bool m_HasEmissiveQuads=false;
    bool m_QuadEncodingValid=false;
    VulkanPipeline m_QuadPipeline;
    VkDescriptorSetLayout m_QuadSetLayout=VK_NULL_HANDLE;
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
    bool m_Texture3DManagerInitialized = false;
    PipelineConfig m_CsmMeshConfig;
    VulkanPipeline m_CsmPipeline;
    VkRenderPass m_CsmRenderPass = VK_NULL_HANDLE;
    std::array<VulkanBuffer, 3> m_CsmInstanceBuffers;
    size_t m_CsmInstanceCapacity = 0;
    std::array<uint64_t, 3> m_CsmInstanceEpochs{UINT64_MAX, UINT64_MAX, UINT64_MAX};
};

#endif
