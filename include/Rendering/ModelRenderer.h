#pragma once
#include "Platform/Export.h"
#ifndef MODEL_RENDERER_H
#define MODEL_RENDERER_H

#include "RendererBase.h"
#include "ModelLoader.h"
#include "TexturePool.h"
#include "AABB.h"
#include "ModelBVH.h"
#include "ECS/Types.h"
#include "ECS/Components.h"
#include "EngineGlobal.h"
#include "DescriptorSetCache.h"
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

struct MIKAN_API VkTexture {
    VkImage image;
    VkImageView imageView;
    VkSampler sampler;
    VkDeviceMemory memory;
};

// 注意：此结构体必须与着色器中的 push constant 布局完全匹配
// 实际 sizeof = 160 字节（64+64+16+16）——shader 侧的材质/alpha push 偏移必须用 160/176（2026-08-17 修正；旧注释 144 是 TAA 加 cameraPosition 前的值）
struct MIKAN_API ModelUniformData {
    glm::mat4 projView;           // 64 bytes（无 jitter——jitter 由 shader 内 clipPos.xy += taaJitter·w 实现，motion 保持纯运动量）
    glm::mat4 prevProjView;       // 64 bytes
    glm::vec3 cameraPosition;     // 12 bytes
    float padding;                // 4 bytes padding for vec3 alignment
    glm::vec2 taaJitter;          // 8 bytes（2026-08-17：TAA 相机亚像素抖动，NDC 偏移；TAA 禁用时 = 0）
    float padding2[2];            // 4 bytes ×2（对齐）
};

struct MIKAN_API ModelPushConstant {
    glm::vec4 albedoColor;
    float metallic;
    float roughness;
    float ao;
    int useAlbedoTexture;
    int useNormalTexture;
    int padding[2];
};

// 注意：此结构体必须与着色器中的顶点输入布局完全匹配
// 使用 vec4 对齐以确保在移动 GPU 上的兼容性
// 总大小: 176 bytes (64 + 64 + 16 + 16 + 16)
struct MIKAN_API ModelInstanceData {
    glm::mat4 model;           // location 5-8 (4 vec4) - offset 0, size 64
    glm::mat4 prevModel;       // location 9-12 (4 vec4) - offset 64, size 64 - 上一帧模型矩阵
    glm::vec4 albedoColor;     // location 13 - offset 128, size 16
    glm::vec4 materialData;    // location 14 - offset 144, size 16 (metallic, roughness, ao, useAlbedoTexture)
    glm::vec4 textureFlags;    // location 15 - offset 160, size 16 (useNormalTexture, 0, 0, 0)
};

// 单个 submesh 的实例绘制批次（submesh 剔除模式下每个可见 submesh 一批，各自带实例列表）
struct MIKAN_API SubMeshInstanceData {
    size_t subMeshIndex = 0;
    std::vector<ModelInstanceData> instances;
};

// ===== 2026-08-17：可见 subMesh 合批（非 MDI）=====
// CPU 加载期按材质键合并同材质 subMesh 的顶点/索引到组 buffer；渲染期组内可见段合并成普通 draw。
// 移动端安全（无 indirect）；材质键含全部影响渲染状态的字段（纹理/环绕/alphaMode/双面/材质参数）。
struct MIKAN_API BatchedSubMeshItem {
    size_t subMeshIndex = 0;
    uint32_t firstIndex = 0;     // 合并索引 buffer 内起始
    uint32_t indexCount = 0;
    uint32_t vertexOffset = 0;   // 索引 baseVertex（顶点拼接偏移）
};
struct MIKAN_API SubMeshBatchGroup {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;   // 组内共享材质 set（同键内容一致，取首 subMesh）
    std::vector<BatchedSubMeshItem> items;            // 按原 subMesh 顺序（buffer 内连续）
};

struct MIKAN_API SubMeshRenderData {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;          // 蒙皮布局顶点缓冲（统一 32B）
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    std::string name;                                // 2026-08-09 subMesh 标识（assimp mesh 名，per-subMesh 材质选择用）
    std::string materialName;
    int wrapMode = 10497;   // 纹理环绕（VkSamplerAddressMode：REPEAT/CLAMP_TO_EDGE；来自 gltf sampler）
    std::string diffuseTexturePath;
    std::string normalTexturePath;
    std::string roughnessTexturePath;
    std::string metallicTexturePath;
    std::string emissiveTexturePath;   // 2026-08-09
    float metallic = -1.0f;    // 2026-08-11 glTF metallicFactor（-1=未设——用实例/默认）
    float roughness = -1.0f;   // 2026-08-11 glTF roughnessFactor（-1=未设）
    float ao = 1.0f;           // 2026-08-11
    float mrValid = 1.0f;      // 2026-08-15：MR 纹理有效性（0=无/占位/全黑纹理→shader 回退默认参数；1=真实有效纹理）
    int alphaMode = -1;        // 2026-08-16 glTF alphaMode：-1=未知(assimp 路径→shader 旧行为) 0=OPAQUE 1=MASK 2=BLEND
    float alphaCutoff = 0.5f;  // 2026-08-16 glTF alphaCutoff（仅 MASK 用）
    bool doubleSided = false;  // 2026-08-16 glTF doubleSided（渲染接入留后续批次）
    float diffuseTransmissionFactor = 0.0f;  // 2026-08-29 显式 glTF 漫反射透射系数；未声明=0
    VulkanDescriptor descriptor;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;           // 蒙皮材质 set（binding 0-4）
    AABB aabb;
};

struct MIKAN_API ModelRenderData {
    std::vector<SubMeshRenderData> subMeshes;
    VulkanPipeline pipeline;
    VulkanPipeline doubleSidedPipeline;  // 双面渲染管线
    VulkanPipeline wireframePipeline;    // 线框渲染管线
    VulkanPipeline depthPipeline;        // z-prepass depth-only（蒙皮 32B）
    VulkanPipeline shadowDepthPipeline;        // 2026-08-13 点光源阴影 depth-only（蒙皮，线性深度 dist/range）
    VulkanPipeline csmDepthPipeline;           // 2026-08-14 CSM 方向光阴影 depth-only（蒙皮，默认 NDC 深度）
    VulkanDescriptor descriptor;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkBuffer uniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uniformBufferMemory = VK_NULL_HANDLE;
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory instanceBufferMemory = VK_NULL_HANDLE;
    size_t instanceBufferSize = 0;
    std::string modelPath;
    glm::vec3 modelCenter = glm::vec3(0.0f);
    glm::vec3 modelMinBounds = glm::vec3(0.0f);
    glm::vec3 modelMaxBounds = glm::vec3(0.0f);
    
    static constexpr size_t MAX_FRAMES_IN_FLIGHT = 3;   // 2026-08-17：2→3（swapchain 三重缓冲后 3 帧 in-flight——双缓冲时帧 N 写 buffer[N%2] 与帧 N-2 GPU 读取竞态 → 黑闪）
    // 保持模型数据结构为纯渲染资源布局。一个 swapchain frame 内可能录制
    // CSM、SceneView、GameView 等多次 draw；这些 draw 的实例上传槽由
    // ModelRenderer.cpp 的外置运行时池管理，避免把可变容器嵌进这个跨模块
    // 共享的数据结构，破坏旧二进制/模块的布局假设。
    VkBuffer instanceBuffers[MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory instanceBufferMemories[MAX_FRAMES_IN_FLIGHT] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* instanceBufferMapped[MAX_FRAMES_IN_FLIGHT] = {nullptr, nullptr};
    size_t currentInstanceBufferSize = 0;
    
    // 排序缓存优化：缓存按描述符集排序的子网格索引
    std::vector<size_t> cachedSortedIndices;
    bool sortedIndicesDirty = true;

    // ===== 2026-08-17：合批组（RebuildBatchGroups 构建；非 MDI——CPU 合并 + 普通 draw）=====
    std::vector<SubMeshBatchGroup> batchGroups;
    std::vector<int> subMeshBatchGroup;   // subMesh → 组索引（-1 = 未合批）

    // ===== 骨骼动画（MVP：模型自带动画加载后自动播放，可代码控制）=====
    bool hasAnimation = false;        // 模型含动画 clip
    int currentClip = 0;              // 当前播放 clip 索引（MeshData::animations）
    float animTime = 0.0f;            // 当前播放时间（秒）
    float animSpeed = 1.0f;           // 播放速度倍率
    bool animLoop = true;             // 循环播放
    bool animPlaying = true;          // 播放开关
    std::vector<glm::mat4> boneMatrices; // 蒙皮矩阵（global * offset），每帧采样后更新
    void* boneBufferMapped = nullptr;     // uniformBuffer 映射指针（骨骼矩阵 buffer）
    VkBufferView boneBufferView = VK_NULL_HANDLE; // 骨骼矩阵 texel buffer 视图（uniform texel buffer）

    // ===== CPU 蒙皮（顶点在 CPU 端加权变换后上传，绕开 GPU 蒙皮 device lost）=====
    bool hasSkinning = false;                       // 模型有骨骼（需 CPU 蒙皮）
    std::vector<std::vector<Vertex>> skinnedSubMeshes; // CPU 蒙皮结果（逐 subMesh）
    VkBuffer skinnedVertexBuffer = VK_NULL_HANDLE;  // host-visible 蒙皮顶点缓冲
    VkDeviceMemory skinnedVertexBufferMemory = VK_NULL_HANDLE;
    void* skinnedBufferMapped = nullptr;
    std::vector<VkDeviceSize> skinnedBufferOffsets; // 每 subMesh 在缓冲内的字节偏移
};

class MIKAN_API ModelRenderer : public BaseRenderer {
public:
    ModelRenderer();
    virtual ~ModelRenderer();

    virtual void Init(VkRenderPass renderPass) override;
    virtual void Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) override;
    virtual void Cleanup() override;

    void LoadModel(const std::string& path);
    // 2026-08-17：按材质键重建合批组（加载后/材质修改后调用；无蒙皮模型才合批）
    void RebuildBatchGroups();
    void DestroyBatchGroups();
    void UpdateSubMeshSampler(size_t subMeshIndex, int textureType, int samplerType);
    // 2026-08 属性面板材质管理：纹理路径+采样器应用到模型全部 subMesh（textureType 0-3，samplerType 0-3）
    void ApplyTextureToAllSubMeshes(int textureType, const std::string& path, int samplerType);
    // 2026-08-09：per-subMesh 材质应用（subMeshIndex 越界/负数=全部）
    void ApplyTextureToSubMesh(int subMeshIndex, int textureType, const std::string& path, int samplerType);
    std::string GetSubMeshName(size_t index) const {
        if (index >= m_ModelData.subMeshes.size()) return {};
        return m_ModelData.subMeshes[index].name.empty()
            ? m_ModelData.subMeshes[index].materialName : m_ModelData.subMeshes[index].name;
    }

    // ===== 骨骼动画接口 =====
    void UpdateAnimation(float deltaTime);      // 推进播放时间 + 采样 + 更新骨骼矩阵 UBO（主循环调用）
    // 应用外部骨骼局部姿态（例如 VMD）。姿态接口保持通用，渲染器不依赖具体动画格式。
    // localTransforms 的顺序必须与 MeshData::bones 一致；调用后立即刷新 GPU/CPU 蒙皮数据。
    bool ApplyBoneLocalPose(const std::vector<glm::mat4>& localTransforms);
    void PlayAnimation(int clipIndex, bool loop); // 切换到指定 clip 并从 0 播放
    bool HasAnimation() const { return m_ModelData.hasAnimation; }
    bool HasSkinning() const { return m_ModelData.hasSkinning; }   // 2026-08-15：阴影缓存判定用（蒙皮场景禁用缓存）
    int GetAnimationCount() const;             // clip 数量
    // Animator 组件同步接口
    void SetAnimationSpeed(float speed) { m_ModelData.animSpeed = speed; }
    void SetAnimationLoop(bool loop)   { m_ModelData.animLoop = loop; }
    void SetAnimationPlaying(bool playing) { m_ModelData.animPlaying = playing; }
    int  GetCurrentClip() const { return m_ModelData.currentClip; }
    float GetAnimationTime() const { return m_ModelData.animTime; }
    bool IsAnimationPlaying() const { return m_ModelData.animPlaying; }
    const char* GetAnimationName() const;      // 当前 clip 名（无动画返回空）

    virtual void CreatePipeline(VkRenderPass renderPass);
    // 2026-08-13 阴影管线复用：CreatePipeline 时缓存顶点绑定/属性描述（蒙皮 + 静态）
    std::vector<VkVertexInputBindingDescription> m_VertexBindings;
    std::vector<VkVertexInputAttributeDescription> m_VertexAttributes;
    void CreateInstanceBuffer(size_t maxInstances);
    void UpdateInstanceBuffer(const std::vector<ModelInstanceData>& instanceData);
    void RenderInstanced(VkCommandBuffer commandBuffer, int width, int height, 
                         const glm::mat4& view, const glm::mat4& proj,
                         const std::vector<ModelInstanceData>& instanceData,
                         const ECS::MaterialComponent* material);
    void RenderInstanced(VkCommandBuffer commandBuffer, int width, int height,
                         const glm::mat4& projView, const glm::mat4& prevProjView,
                         const glm::vec3& cameraPosition,
                         const std::vector<ModelInstanceData>& instanceData,
                         const ECS::MaterialComponent* material,
                         const std::vector<size_t>& visibleSubMeshIndices);
    // 批量渲染多个 submesh（每个 submesh 各自实例列表）：一次管线/viewport/push constant 绑定，
    // 内部按材质(描述符集)分组，消除 submesh 剔除模式下"每 submesh 一次调用"的重复状态切换。
    void RenderInstancedBatches(VkCommandBuffer commandBuffer, int width, int height,
                                const glm::mat4& projView, const glm::mat4& prevProjView,
                                const glm::vec3& cameraPosition,
                                const std::vector<SubMeshInstanceData>& batches,
                                const ECS::MaterialComponent* material,
                                bool doubleSided, bool wireframe);
    void RenderInstancedDoubleSided(VkCommandBuffer commandBuffer, int width, int height,
                         const glm::mat4& projView, const glm::mat4& prevProjView,
                         const glm::vec3& cameraPosition,
                         const std::vector<ModelInstanceData>& instanceData,
                         const ECS::MaterialComponent* material,
                         const std::vector<size_t>& visibleSubMeshIndices);
    void RenderInstancedWireframe(VkCommandBuffer commandBuffer, int width, int height,
                         const glm::mat4& projView, const glm::mat4& prevProjView,
                         const glm::vec3& cameraPosition,
                         const std::vector<ModelInstanceData>& instanceData,
                         const ECS::MaterialComponent* material,
                         const std::vector<size_t>& visibleSubMeshIndices);
    // z-prepass（subpass 0，depth-only）：只写深度，无材质采样——MRT 阶段被遮挡片元在 fragment shader 前被剔除
    void RenderDepthOnly(VkCommandBuffer commandBuffer, int width, int height,
                         const glm::mat4& projView,
                         const std::vector<ModelInstanceData>& instanceData,
                         const std::vector<size_t>& visibleSubMeshIndices = {});   // 2026-08-09：非空时只绘制可见 subMesh（z-prepass 剔除）
    // 2026-08-13 点光源阴影：线性深度（frag 写 dist/range），push = projView + lightPosRange（80B）
    // 阴影管线（shadowDepth/shadowStaticDepthPipeline）由 EnsureShadowPipelines 惰性创建（render pass = 阴影 depth-only pass）
    void EnsureShadowPipelines(VkRenderPass shadowRenderPass);
    void RenderShadowDepth(VkCommandBuffer commandBuffer, int width, int height,
                           const glm::mat4& projView, const glm::vec3& lightPos, float range,
                           const std::vector<ModelInstanceData>& instanceData,
                           const std::vector<size_t>& visibleSubMeshIndices = {});
    // 2026-08-14 CSM 方向光阴影：默认 NDC 深度（正交线性），顶点复用 shadow_depth 系列，frag = csm_depth.frag（空）
    // 管线带 depth bias（防 acne）；push = projView + lightPosRange（80B，与 shadow 管线兼容布局，frag 忽略 lightPosRange）
    void EnsureCsmPipelines(VkRenderPass csmRenderPass);
    void RenderCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                        const glm::mat4& projView,
                        const std::vector<ModelInstanceData>& instanceData,
                        const std::vector<size_t>& visibleSubMeshIndices = {});

    const std::string& GetModelPath() const { return m_ModelData.modelPath; }
    bool HasAlbedoTexture() const;
    bool HasNormalTexture() const;
    bool HasEmissiveTexture() const;   // 2026-08-09
    bool HasRoughnessTexture() const;   // 2026-08-09
    bool HasDoubleSided() const;   // 2026-08-17：任一 subMesh 材质 doubleSided（SceneRenderer 分派走双面管线）
    bool HasMetallicTexture() const;   // 2026-08-09
    const std::string& GetAlbedoTexturePath() const;
    
    bool HasModelLoaded() const { return !m_MeshData.subMeshes.empty(); }
    AABB GetAABB() const;
    std::vector<AABB> GetSubMeshAABBs() const;
    
    size_t GetVertexCount() const;
    size_t GetSubMeshCount() const;
    
    std::vector<SubMeshRenderData>& GetSubMeshes() { return m_ModelData.subMeshes; }

    const MeshData& GetMeshData() const { return m_MeshData; }
    
    virtual VkPipeline GetPipeline() const override { return m_ModelData.pipeline.GetPipeline(); }
    virtual VkPipelineLayout GetPipelineLayout() const override { return m_ModelData.pipeline.GetLayout(); }
    VkPipeline GetDoubleSidedPipeline() const { return m_ModelData.doubleSidedPipeline.GetPipeline(); }
    VkPipeline GetWireframePipeline() const { return m_ModelData.wireframePipeline.GetPipeline(); }
    
    const ModelBVHData& GetBVHData() const { return m_BVHData; }
    ModelBVHData& GetBVHData() { return m_BVHData; }
    bool HasBVH() const { return m_BVHData.IsValid(); }
    bool HasTopLevelBVH() const { return m_BVHData.HasTopLevelBVH(); }
    std::vector<AABB> GetBVHNodeBounds() const;
    std::vector<AABB> GetBVHNodeBounds(size_t subMeshIndex) const;
    std::vector<AABB> GetTopLevelBVHNodeBounds() const;

protected:
    void CreateModelBuffers(const MeshData& meshData);
    void CalculateAABB();
    void CreateUniformBuffer();
    void SetupDescriptorSets();
    void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    void BuildSortedIndices();
    void RefreshBoneMatricesAndSkinning();

    ModelRenderData m_ModelData;
    MeshData m_MeshData;
    ModelBVHData m_BVHData;
    std::unordered_map<std::string, VkTexture> m_TextureCache;
    TexturePool* m_TexturePool;
};

#endif
