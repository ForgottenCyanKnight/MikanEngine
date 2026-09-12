#pragma once
#include "Platform/Export.h"

#include "RendererBase.h"
#include "ModelRenderer.h"
#include "VoxRenderer.h"
#include "VoxelMeshMultiDrawIndirect.h"
#include "WireframeRenderer.h"
#include "SceneDebugRenderer.h"
#include "SceneTypes.h"
#include "SceneCollector.h"
#include "OcclusionCulling.h"
#include "PointShadowRenderer.h"
#include "CascadeShadowRenderer.h"
#include "CameraUniformBuffer.h"
#include "RenderFrameContext.h"
#include "QuadTreeCulling.h"
#include "ECS/Types.h"
#include "ECS/Components.h"
#include "ComputeShader.h"
#include "FullscreenQuad.h"
#include "HiZComputeShader.h"
#include "TerrainRenderer.h"
#include "WaterRenderer.h"
#include "World/WorldRenderer.h"

#include <vector>
#include <limits>
#include <glm/glm.hpp>
#include <unordered_map>
#include <unordered_set>
#include <iostream>



class MIKAN_API SceneRenderer : public BaseRenderer {
public:
    static SceneRenderer* GetInstance();
    
    SceneRenderer();
    virtual ~SceneRenderer();

    virtual void Init(VkRenderPass renderPass) override;
    virtual void Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) override;
    virtual void Cleanup() override;
    
    virtual VkPipeline GetPipeline() const override { return VK_NULL_HANDLE; }
    virtual VkPipelineLayout GetPipelineLayout() const override { return VK_NULL_HANDLE; }

    // 统一视图描述（Camera+Viewport 抽象）：
    // 不同窗口只是不同视图描述的输出反馈；渲染器不区分"编辑器/游戏"，只消费视图描述。
    // 编辑器特化（gizmo/线框等）由 editorFeatures 控制，且仅当 Editor.dll 存在时启用。
    enum class ViewRenderMode {
        Game,        // 游戏视图：主相机 + 后处理 + Hi-Z
        EditorScene  // 编辑器场景视图：编辑器相机 + 调试渲染（无后处理）
    };

    // 每帧无条件调用：场景模式判定（仅 2D 相机且无主 3D 相机 → g_SceneIs2D=true）
    // 必须在任何 3D/2D 渲染分支判断之前调用（历史 bug：判定曾藏在 PrepareFrame，2D 场景不经过它，
    // 导致 2D→3D 场景切换后 g_SceneIs2D 卡在 true，3D 场景无画面）
    void UpdateSceneMode();

    // 帧渲染唯一入口：RenderECS 主干 = 一行一个 pass 的清单（各 pass 的输入/输出/依赖注释见 cpp 定义处）。
    // 仅由 RenderSceneView / RenderGameView 调用。
    void RenderECS(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj, const glm::mat4& cullView, const glm::mat4& cullProj, VulkanBuffer& uniformBuffer, VkDescriptorSet descriptorSet, ViewRenderMode mode);
    
    VoxelMeshMultiDrawIndirect* GetVoxelMDI() const { return m_VoxelMeshMultiDrawIndirect.get(); }
    // 由 RenderUIOverlay 在 SceneView 链末调用；EnsureInit 惰性绑定 UI pass；仅编辑器场景视图
    void RenderOverlayLinework(VkCommandBuffer commandBuffer, int width, int height,
                               VkRenderPass uiPass, const glm::mat4& view, const glm::mat4& proj);

    void RenderSceneView(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj);
    void RenderSceneView(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj, const glm::mat4& cullView, const glm::mat4& cullProj);
    void RenderGameView(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj);
    // z-prepass（render pass subpass 0，depth-only）：只写 3D 深度，MRT 几何阶段（subpass 1）被遮挡片元在 fragment shader 前剔除。
    // 由 VulkanManager 在 BeginRender 后、NextSubpass 前调用（2D 场景跳过）。
    void RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                        const glm::mat4& view, const glm::mat4& proj,
                        bool useMainCameraFrustum = false);

    // 内部惰性创建 PointShadowRenderer + ModelRenderer::EnsureShadowPipelines；几何收集同 RenderDepthPrepass（无剔除）
    struct ShadowLight { glm::vec3 position; float range; };
    void RenderPointShadowMaps(VkCommandBuffer commandBuffer, const ShadowLight* lights, int lightCount, int shadowMapSize);
    PointShadowRenderer* EnsurePointShadows();
    // 内部惰性创建 CascadeShadowRenderer + ModelRenderer::EnsureCsmPipelines；几何收集同点阴影（模型分组 + 级联视锥剔除）
    // slot：0=SceneView/游戏模式，1=GameView；lightDir 从场景指向光源（合成 shader pc.sunDir 同语义）
    CascadeShadowRenderer* EnsureCascadeShadows();
    void RenderCascadeShadowMaps(VkCommandBuffer commandBuffer, int slot,
                                 const glm::mat4& view, const glm::mat4& proj, const glm::vec3& lightDir);
    
    // 返回当前场景中标记为主相机且具备 Transform 的实体。
    ECS::Entity GetMainCameraEntity();
    bool GetMainCameraMatrices(float aspectRatio, glm::mat4& outView, glm::mat4& outProj, glm::vec3& outCameraPos);
    
    // 获取模型渲染器
    ModelRenderer* GetModelRenderer(const std::string& modelPath);
    size_t GetModelRendererCount() const { return m_ModelRenderers.size(); }
    ModelRenderer* GetModelRendererByIndex(size_t index);

    // 骨骼动画：推进所有模型渲染器的播放时间 + 采样 + 更新蒙皮矩阵 UBO（主循环播放态调用）
    void UpdateModelAnimations(float deltaTime);
    
    // 获取体素渲染器
    VoxRenderer* GetVoxRenderer(const std::string& voxPath);
    size_t GetVoxRendererCount() const { return m_VoxRenderers.size(); }
    bool HasVoxRenderer(const std::string& voxPath) const;
    
    // 获取计算着色器
    HiZComputeShader& GetHiZShader() { return m_HiZShader; }
    
    // 获取全屏四边形
    FullscreenQuad& GetFullscreenQuad() { return m_FullscreenQuad; }

    // 体素世界渲染器（从 OpenGL 版迁移；由 WorldSystem 提供 World 数据）
    WorldRenderer* GetWorldRenderer() const { return m_WorldRenderer.get(); }
    void SetWorldRendererEnabled(bool enabled) { m_WorldRenderEnabled = enabled; }
    bool IsWorldRenderEnabled() const { return m_WorldRenderEnabled; }
    TerrainRenderer& GetTerrainRenderer() { return m_TerrainRenderer; }

    
    // 获取上一帧 ProjView 矩阵（用于运动矢量计算）
    const glm::mat4& GetPrevProjViewMatrix() const { return m_PrevProjViewMatrix; }
    bool HasPrevFrameMatrices() const { return m_HasPrevFrameMatrices; }

    
    // 获取场景光源数量
    int GetLightCount() const { return m_LightCount; }
    
    
    // Hi-Z 深度金字塔控制
    bool IsHiZCullingEnabled() const { return m_EnableHiZCulling; }
    void SetHiZCullingEnabled(bool enabled) { m_EnableHiZCulling = enabled; }
    
    
   
    // 预加载场景中的所有模型
    void PreloadModels();
    
    glm::vec3 GetCameraPosition();

    // ---- 显式 pass 清单（RenderECS 按此顺序调用）----
    // Pass 1 (prepare): 场景收集 / 相机 / 光源 / 剔除状态 / 四叉树构建。纯 CPU，不发 GPU 命令。
    //   输入: ctx 基础字段(commandBuffer/view/proj/cullView/cullProj/uniformBuffer/descriptorSet/isSceneView)
    //   输出: ctx 收集结果(modelGroups/voxGroups/rootEntities/cameraPos/剔除状态) + m_DebugRenderer 中的调试线
    //   依赖: 无（帧内第一个 pass）
    void PrepareFrame(RenderFrameContext& ctx);
    // Pass 2 (geometry): 不透明几何（模型 + 静态体素 MDI + 动态体素 + 无限体素世界）。
    //   输入: PrepareFrame 填充的 ctx
    //   输出: 离屏目标颜色/深度；更新 m_PrevProjViewMatrix / m_PrevModelMatrices（供运动矢量）
    //   依赖: Hi-Z 遮挡剔除数据由 VulkanManager 在调用本 pass 前生成（GenerateMipLevels）
    void RenderGeometryOpaque(RenderFrameContext& ctx);
    // Pass 3 (debug overlay): 场景视图的摄像机视锥线框，叠加在几何之上。
    //   输入: PrepareFrame 期间收集到 m_DebugRenderer 的视锥实例
    //   输出: 向 commandBuffer 追加绘制
    //   依赖: 在 RenderGeometryOpaque 之后（画在几何上方）；后处理呈现由 VulkanManager 在离屏渲染后执行
    
    
    // 四叉树可视化
    bool IsShowQuadTree() const { 
        return m_ShowQuadTree; 
    }
    void SetShowQuadTree(bool show) { 
        m_ShowQuadTree = show; 
    }
    
private:
    VulkanBuffer m_SceneUniformBuffer;
    VkDescriptorSet m_SceneDescriptorSet = VK_NULL_HANDLE;
    
    VulkanBuffer m_GameUniformBuffer;
    VkDescriptorSet m_GameDescriptorSet = VK_NULL_HANDLE;

    void CollectModelEntities(ECS::Entity entity, std::vector<ECS::Entity>& modelEntities);
    void CollectModelEntitiesByPath(ECS::Entity entity, std::unordered_map<std::string, ModelInstanceGroup>& modelGroups);
    void CollectVoxModelEntitiesByPath(ECS::Entity entity, std::unordered_map<std::string, VoxInstanceGroup>& voxGroups);
    void CollectLightEntities(ECS::Entity entity, std::vector<ECS::Entity>& lightEntities);
    void CollectCameraEntities(ECS::Entity entity, std::vector<ECS::Entity>& cameraEntities);
    
    std::unordered_map<std::string, std::unique_ptr<ModelRenderer>> m_ModelRenderers;
    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    
    // 体素渲染器
    std::unordered_map<std::string, std::unique_ptr<VoxRenderer>> m_VoxRenderers;
    // 体素模型 MDI 渲染器
    std::unique_ptr<VoxelMeshMultiDrawIndirect> m_VoxelMeshMultiDrawIndirect;
    
    // 体素世界渲染器（无限体素世界，从 OpenGL 版迁移）
    std::unique_ptr<WorldRenderer> m_WorldRenderer;
    // 高度图地形（固定 patch + chunk 实例 + LOD）
    TerrainRenderer m_TerrainRenderer;
    // 水体（共享三角形条带网格 + 实例流；第一阶段不透明）
    WaterRenderer m_WaterRenderer;
    std::unique_ptr<PointShadowRenderer> m_PointShadows;
    std::unique_ptr<CascadeShadowRenderer> m_CascadeShadows;
    bool m_WorldRenderEnabled = true;
    
    
    // 四叉树剔除上下文
    Culling::CullingContext m_CullingContext;
    bool m_QuadTreeDirty = true;

    std::array<Plane, 6> m_MainCameraFrustumPlanes = {};
    bool m_HasMainCameraFrustum = false;
    bool m_MainCamUseSubMeshCulling = true;
    
    // 摄像机位置缓存，用于检测摄像机移动
    glm::vec3 m_LastCameraPos = glm::vec3(FLT_MAX);
    float m_CameraMoveThreshold = 1.0f; // 移动阈值

    // 相机实体列表帧缓存:GetMainCameraMatrices / GetCameraPosition / PrepareFrame 共用,
    // 按帧 ID + 实体集合版本惰性重建,把每帧多次全树相机收集降为最多一次。
    std::vector<ECS::Entity> m_CameraEntitiesCache;
    uint64_t m_CameraCacheFrameId = std::numeric_limits<uint64_t>::max(); // 初始无效,保证第一帧必收集
    uint64_t m_FrameId = 0;                                               // 每帧 PrepareFrame 递增
    uint32_t m_CameraCacheSceneVersion = 0;                              // 收集时的实体集合版本(重载场景后失效)
    // 确保相机缓存对本帧有效(帧号或实体集合变化则重新收集并标记本帧)
    const std::vector<ECS::Entity>& EnsureCameraEntitiesCached();
    
    // 模型数量缓存，用于检测场景变化
    size_t m_LastModelCount = 0;
    
    // 四叉树可视化
    bool m_ShowQuadTree = false;
    // Editor/debug wireframe visualization (AABB/OBB/BVH), split out of the renderer
    SceneDebugRenderer m_DebugRenderer;
    SceneDebugRenderer& GetDebugRenderer() { return m_DebugRenderer; }

    // Visibility culling (quadtree + occlusion + BVH submesh)
    OcclusionCulling m_OcclusionCulling;
    OcclusionCulling& GetOcclusionCulling() { return m_OcclusionCulling; }

    // Camera uniform buffer
    CameraUniformBuffer m_CameraUniformBuffer;
    
    // 场景光源数量
    int m_LightCount = 0;

    // 全屏四边形（用于显示计算着色器输出）
    FullscreenQuad m_FullscreenQuad;


    
    // 上一帧的 ProjView 矩阵（用于运动矢量计算）
    glm::mat4 m_PrevProjViewMatrix = glm::mat4(1.0f);
    bool m_HasPrevFrameMatrices = false;
    
    // 上一帧的模型矩阵（用于运动矢量计算）
    std::unordered_map<ECS::Entity, glm::mat4> m_PrevModelMatrices;
    // 场景实体集合变化后，旧 Entity 索引可能已经被复用；历史矩阵必须
    // 在新场景的第一帧清空，避免 TAA/运动矢量读取到旧实体的 transform。
    uint32_t m_PrevModelMatricesSceneVersion = std::numeric_limits<uint32_t>::max();
    
    // Hi-Z 深度金字塔
    HiZComputeShader m_HiZShader;
    bool m_EnableHiZCulling = true;
};
