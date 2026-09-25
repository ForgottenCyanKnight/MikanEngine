#pragma once

#include "Platform/Export.h"
#include "AABB.h"
#include "RendererBase.h"
#include "ECS/Components.h"
#include "ECS/Types.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct RenderWorld;

// 水位图的最大水深（米）：水位笔刷的 1.0 归一化深度对应的实际水深。
// 编辑器 UI（TerrainBrushTool.h 侧有一份同值常量）与地形着色器共用该语义，
// 修改时三处必须同步。
inline constexpr float kTerrainWaterMaxDepth = 8.0f;

// 地形采用“固定 patch 顶点 + chunk 实例”的输入布局。
// Y 由顶点着色器从 16-bit heightmap 采样得到，避免每个 chunk 重复存储高度顶点。
// UV 直接由 position 推导；LOD 边界在顶点着色器折叠，不生成地下裙边，因此顶点保持 8 bytes。
struct MIKAN_API TerrainVertex {
    glm::vec2 position = glm::vec2(0.0f); // patch 内归一化 X/Z 坐标 [0, 1]
};

// 每个实例代表一个可见 terrain chunk。
// originSize.xy 是地形局部空间的 X/Z 原点，originSize.zw 是 chunk 尺寸。
// uvRect.xy 保存整数 chunk 网格坐标 (x, z)，uvRect.z 保存 chunkCount；
// 顶点着色器用它重建全局 [0, 1] 坐标，避免相邻 chunk 分别做
// "原点 + 尺寸" 浮点运算后在边界产生不同结果。
struct MIKAN_API TerrainChunkInstance {
    glm::vec4 originSize = glm::vec4(0.0f); // localOrigin.x, localOrigin.z, size.x, size.z
    glm::vec4 uvRect = glm::vec4(0.0f);     // chunkX, chunkZ, chunkCount, unused
    glm::vec4 params = glm::vec4(0.0f);     // lod, packed edge LOD deltas, edge intervals, reserved
};

static_assert(sizeof(TerrainVertex) == sizeof(float) * 2, "TerrainVertex must stay 8 bytes");
static_assert(sizeof(TerrainChunkInstance) == sizeof(float) * 12, "TerrainChunkInstance must stay 48 bytes");

// GPU terrain-MDI 的细粒度 tile 描述。bounds 已经是世界空间 AABB，params.x
// 保存该 tile 的 LOD（0..2），params.y 是 CPU 候选流中的 LOD 索引，
// params.z 保存该 LOD patch 的 indexCount，供 GPU 压缩后的命令初始化/校验。
// CPU 先压缩视锥/距离候选，compute 再做 GPU 细筛。
// std430 与 terrain_cull.comp 保持 48 字节布局。
struct MIKAN_API TerrainMdiTile {
    glm::vec4 minBounds = glm::vec4(0.0f);
    glm::vec4 maxBounds = glm::vec4(0.0f);
    glm::uvec4 params = glm::uvec4(0u);
};

static_assert(sizeof(TerrainMdiTile) == sizeof(float) * 12,
              "TerrainMdiTile must stay 48 bytes");

// 一株草叶实例。CPU 侧按草密度图散布生成，叶片几何本身由顶点着色器
// 用二次贝塞尔曲线程序化生成（条带拓扑、零顶点缓冲），因此实例只需
// 携带位置与形状参数，全部 32 字节对齐 16 字节。
//   posParams   = (局部X, 局部Z, 朝向角 yaw, 叶高 h)
//   shapeParams = (叶宽 w, 弯曲量 bend, 风摆相位 phase, 色调 tint)
// Y 不存储：顶点着色器每帧从 16-bit 高度图采样，雕刻地形后草自动贴地。
struct MIKAN_API GrassBladeInstance {
    glm::vec4 posParams = glm::vec4(0.0f);
    glm::vec4 shapeParams = glm::vec4(0.0f);
};

static_assert(sizeof(GrassBladeInstance) == sizeof(float) * 8, "GrassBladeInstance must stay 32 bytes");

// 草实例按地形 chunk 网格分桶的桶描述：实例流按桶连续存放（桶序 = 线性网格序），
// 渲染时逐桶做视锥测试，可见桶各发一次 vkCmdDraw(firstInstance)。
struct MIKAN_API GrassChunkBucket {
    uint32_t firstInstance = 0; // 桶在草实例流中的起始下标
    uint32_t count = 0;         // 桶内实例数
    AABB localBounds;           // 地形局部空间包围盒（按高度图逐桶取紧凑 Y 范围 + 叶高余量）
};

struct MIKAN_API TerrainChunk {
    int x = 0;
    int z = 0;
    glm::vec2 origin = glm::vec2(0.0f);
    glm::vec2 size = glm::vec2(0.0f);
    glm::vec2 uvOrigin = glm::vec2(0.0f);
    glm::vec2 uvSize = glm::vec2(0.0f);
    AABB localBounds;
};

// CPU 侧 chunk 管理：规则网格、距离/视锥裁剪和三档 LOD 分类。
// chunk 本身不持有 GPU 资源，因此可以在编辑器属性改变时低成本重建。
class MIKAN_API TerrainChunkManager {
public:
    void Configure(const glm::vec2& worldSize, int chunkCount,
                   float minHeight, float maxHeight,
                   float viewDistance, float lod0Distance, float lod1Distance,
                   int maxLod);

    void UpdateVisibility(const glm::vec3& cameraPosition,
                          const glm::mat4& modelMatrix,
                          const std::array<Plane, 6>& frustumPlanes,
                          bool useFrustumCulling,
                          int viewSlot = 0);

    const std::vector<TerrainChunk>& GetChunks() const { return m_Chunks; }
    const std::vector<TerrainChunkInstance>& GetVisible(int lod) const;
    size_t GetVisibleCount() const;
    int GetChunkCount() const { return m_ChunkCount; }

private:
    std::vector<TerrainChunk> m_Chunks;
    struct VisibilityCache {
        bool valid = false;
        bool useFrustumCulling = false;
        glm::vec3 cameraPosition = glm::vec3(0.0f);
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        std::array<Plane, 6> frustumPlanes{};
        std::array<std::vector<TerrainChunkInstance>, 3> visible;
    };
    // 每个视图槽一段可见集缓存。必须容得下反射探针（段 2）——早期这里是 2 段，
    // 探针的槽位被夹到 1，直接覆盖游戏视图的可见集。
    std::array<VisibilityCache, 3> m_VisibilityCaches{};
    int m_ActiveVisibilitySlot = 0;
    glm::vec2 m_WorldSize = glm::vec2(1.0f);
    int m_ChunkCount = 1;
    float m_ViewDistance = 2000.0f;
    float m_Lod0Distance = 200.0f;
    float m_Lod1Distance = 500.0f;
    int m_MaxLod = 2;
};

struct MIKAN_API TerrainPatch {
    VulkanBuffer vertexBuffer;
    VulkanBuffer indexBuffer;
    uint32_t indexCount = 0;
    uint32_t resolution = 0;

    void Cleanup() {
        vertexBuffer.Cleanup();
        indexBuffer.Cleanup();
        indexCount = 0;
        resolution = 0;
    }
};

// Terrain UBO：当前/上一帧矩阵和预计算法线矩阵用于稳定运动矢量与非均匀缩放，
// 高度/材质参数保持 16-byte 对齐。
struct MIKAN_API TerrainUniformData {
    glm::mat4 projView = glm::mat4(1.0f);
    glm::mat4 prevProjView = glm::mat4(1.0f);
    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 prevModel = glm::mat4(1.0f);
    glm::mat4 normalMatrix = glm::mat4(1.0f);
    glm::vec4 heightParams = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
    glm::vec4 materialParams = glm::vec4(1.0f, 0.0f, 1.0f, 0.0f);
    glm::vec4 cameraPosition = glm::vec4(0.0f);
    glm::vec4 taaJitter = glm::vec4(0.0f);
    // 草地渲染参数（尾部追加：terrain 着色器不读，grass 着色器读）。
    // x = 时间(秒)，y = 风强，z = 草可见距离(米)，w = 草叶高度增益。
    glm::vec4 timeWind = glm::vec4(0.0f, 1.0f, 220.0f, 1.0f);
};

static_assert(sizeof(TerrainUniformData) % 16 == 0, "TerrainUniformData must be 16-byte aligned");

class MIKAN_API TerrainRenderer {
public:
    TerrainRenderer() = default;
    ~TerrainRenderer();
    TerrainRenderer(const TerrainRenderer&) = delete;
    TerrainRenderer& operator=(const TerrainRenderer&) = delete;
    TerrainRenderer(TerrainRenderer&&) = delete;
    TerrainRenderer& operator=(TerrainRenderer&&) = delete;

    // renderPass 必须是当前 SceneRenderTarget 的 render pass；swapchain 重建时可重复调用。
    void Init(VkRenderPass renderPass);
    void Cleanup();

    // 在 geometry pass 前完成场景遍历、资源准备和 chunk 可见性更新。
    void Prepare(const std::vector<ECS::Entity>& rootEntities,
                const glm::vec3& cameraPosition,
                const std::array<Plane, 6>& frustumPlanes,
                bool useFrustumCulling,
                int viewSlot = 0);

    // Snapshot-driven preparation used by the frame renderer.  The legacy
    // rootEntities overload remains for tools that still own ECS extraction.
    void Prepare(const RenderWorld& world,
                 const glm::vec3& cameraPosition,
                 const std::array<Plane, 6>& frustumPlanes,
                 bool useFrustumCulling,
                 int viewSlot = 0);

    // z-prepass 早于 SceneRenderer::PrepareFrame，因此提供独立的场景收集入口。
    void PrepareFromScene(const glm::vec3& cameraPosition,
                          const std::array<Plane, 6>& frustumPlanes,
                          bool useFrustumCulling,
                          int viewSlot = 0);

    // viewSlot / probeFace：本视图的资源槽位与探针面序号。
    // viewSlot = 0=场景视图 / 1=游戏视图 / 2=反射探针（探针再按 probeFace 0..5 分段）。
    // 探针会走自己的相机 UBO，见 Resource::probeUniformBuffers 的注释。
    void Render(VkCommandBuffer commandBuffer, int width, int height,
                const glm::mat4& projView,
                const glm::mat4& prevProjView,
                const glm::vec3& cameraPosition,
                int viewSlot = 0,
                int probeFace = 0);

    void RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                            const glm::mat4& projView,
                            const glm::vec3& cameraPosition,
                            int viewSlot = 0,
                            int probeFace = 0);

    // CSM depth-only caster path. The CSM render pass is owned by
    // CascadeShadowRenderer, so this pipeline is created lazily for that pass.
    bool EnsureCsmDepthPipeline(VkRenderPass shadowRenderPass);

    // ===== 水面目标 RT（deferred water compositing）=====
    // 惰性创建水面目标管线（共享 WaterTargetRT 的 render pass，SceneRenderer
    // 持有并在 Init 时传入）。地形水不再写 G-buffer。
    bool EnsureWaterTargetPipeline(VkRenderPass waterTargetRenderPass);
    // 在共享 WaterTargetRT 的活动 render pass 内绘制 prepared 地形的水面网格
    //（SceneRenderer::RenderWaterTargets 统一编排：EnsureSize→BeginPass→
    // 地形水→实体水→EndPass）。per-frame UBO 由本帧 RenderInternal 已写入。
    void DrawWaterToTarget(VkCommandBuffer commandBuffer, int width, int height,
                           const glm::mat4& projView);
    bool HasPreparedTerrain() const { return !m_PreparedResources.empty(); }
    // 草的 CSM 深度管线（复用 grass.vert + 空片元），让草向阴影图投影。
    bool EnsureGrassDepthPipeline(VkRenderPass shadowRenderPass);
    void RenderCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                        const glm::mat4& shadowProjView,
                        const glm::vec3& cameraPosition);
    // CSM 深度 pass 里绘制草（在 terrain RenderCsmDepth 之后调用）。
    // 复用 grass.vert：草影与主 pass 草几何/风摆/LOD 严格一致。
    // mainCameraFrustum = 主相机视锥（SceneShadowPass 用 view*proj 现场提取）：
    // 草影收集只取主相机视锥内的桶——级联光视锥比相机视锥宽，视野外的草
    // 不再画影子（用户拍板：近两级联 + 视锥内收集）。
    void RenderGrassCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                             const glm::mat4& shadowProjView,
                             const glm::vec3& cameraPosition,
                             const std::array<Plane, 6>& mainCameraFrustum);

    // ===== 草地 GPU 逐桶剔除（阶段一）=====
    // 记录当前视图的草剔除 compute：逐桶 AABB×视锥 + 水平距离测试，把
    // VkDrawIndirectCommand（不可见桶 instanceCount=0）写进命令 SSBO 的
    // 对应视图段；主 pass 草绘制随后用一条 vkCmdDrawIndirect 消费该段。
    // 必须在 render pass 之外调用（SceneShadowPass 的 CSM 准备段是每帧
    // 唯一同时持有视图矩阵又在 pass 外的钩子点）。
    // viewSlot 与 CSM slot 同语义：0 = 场景视图/移动端游戏相机，
    // 1 = 编辑器游戏视图相机（kGameCsmSlot）；越界 slot 直接忽略（走 CPU 回退）。
    void RecordGrassGpuCull(VkCommandBuffer commandBuffer,
                            const glm::mat4& view, const glm::mat4& proj,
                            int viewSlot);

    // 叶片级剔除录制入口（public：帧管线在 CSM 阴影 pass 之前直接调用）。
    // 必须在 render pass 之外调用——vkCmdDispatch 在 render pass 内会被驱动
    // 静默忽略。每帧每视图各一次，viewSlot 与 CSM slot 语义对齐（0=场景视图/
    // 移动端游戏、1=编辑器游戏视图）。
    void RecordGrassBladeCull(VkCommandBuffer commandBuffer, const glm::mat4& view,
                              const glm::mat4& proj, int viewSlot);

    // 记录 terrain 4x4 tile 的 GPU 视锥/距离剔除。每个 LOD 使用一段
    // VkDrawIndexedIndirectCommand，主/深度 pass 随后各用一条 MDI 绘制。
    // 任意前置条件不满足时 RenderInternal 自动走原有 CPU chunk 路径。
    void RecordTerrainGpuCull(VkCommandBuffer commandBuffer,
                              const glm::mat4& view, const glm::mat4& proj,
                              int viewSlot);

    bool IsInitialized() const { return m_Pipeline.GetPipeline() != VK_NULL_HANDLE; }
    size_t GetTerrainCount() const { return m_Resources.size(); }
    size_t GetVisibleChunkCount() const { return m_VisibleChunkCount; }

    // ===== 编辑器地形笔刷 =====
    // 以下接口都按 ECS Entity 定位资源，要求该实体在本帧 Prepare 后已建好地形资源。
    // 射线、采样点与半径都用世界空间；内部经模型矩阵逆变换到地形局部空间。
    //
    // 射线与地形高度面求交（含 AABB 粗判 + 步进 + 二分细化），命中返回 true。
    bool RaycastTerrainWorld(ECS::Entity entity,
                             const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                             glm::vec3& outWorldHit) const;
    // 取世界 XZ 处的地表世界高度（笔刷圆圈贴合地表用）。
    bool SampleTerrainWorldHeight(ECS::Entity entity, float worldX, float worldZ,
                                  float& outWorldY) const;
    // 在半径内按 smoothstep 平滑衰减抬高/降低高度图，并只把受影响的矩形区域
    // 局部上传到 GPU。normalizedDelta 是归一化高度增量（0..1 尺度，正=升高）。
    bool SculptTerrainWorld(ECS::Entity entity, float worldX, float worldZ, float radius,
                            float normalizedDelta);
    // 高度图尺寸与是否为程序化平坦地形（属性面板显示用）。
    bool GetHeightmapInfo(ECS::Entity entity, uint32_t& outWidth, uint32_t& outHeight,
                          bool& outProcedural) const;

    // ===== 编辑器地形材质笔刷 =====
    // 在控制图（RGBA8 图层权重图）上涂抹：沿笔刷半径把该 texel 的四通道权重
    // 向 "layerIndex 独热" 混合，因此涂出来的是一块纯材质并带可控的过渡边界。
    // layerIndex 0..3 对应 layer0..layer3；amount 是笔刷中心处本次调用的混合量(0..1)。
    // hardness(0..1) 决定过渡边界的软硬：
    //   1 = 平顶核心几乎占满半径，只有外缘一条极窄的带里做渐变（硬边）；
    //   0 = 从笔刷中心到边缘全程渐变（最软）。
    bool PaintTerrainMaterialWorld(ECS::Entity entity, float worldX, float worldZ, float radius,
                                   int layerIndex, float hardness, float amount);
    // 控制图尺寸、是否程序化生成、以及是否可涂抹（有 CPU 镜像才可涂）。
    bool GetControlMapInfo(ECS::Entity entity, uint32_t& outWidth, uint32_t& outHeight,
                           bool& outProcedural, bool& outPaintable) const;

    // ===== 编辑器草地笔刷 =====
    // 在草密度图（R8，与高度图同分辨率）上涂抹：沿笔刷半径把 texel 密度
    // 向 targetDensity(0..1) 混合。hardness 语义与材质笔刷一致（过渡软硬），
    // amount 是本次调用的混合量(0..1)。散布实例在下一帧 Prepare 时重建。
    bool PaintTerrainGrassWorld(ECS::Entity entity, float worldX, float worldZ, float radius,
                                float targetDensity, float hardness, float amount);
    // 草密度图尺寸与是否可涂（属性面板提示用）。
    bool GetGrassMapInfo(ECS::Entity entity, uint32_t& outWidth, uint32_t& outHeight,
                         bool& outPaintable) const;

    // ===== 编辑器水位笔刷 =====
    // 在水位图（R8，与高度图同分辨率）上涂抹：沿笔刷半径把 texel 水深
    // 向 targetDepth(0..1) 混合。1.0 对应 kTerrainWaterMaxDepth 米。
    // hardness/amount 语义与草地图笔刷一致。渲染侧当前是地形片元着色器里的
    // 不透明水面 mask（占位实现），以后再升级为独立水面网格。
    bool PaintTerrainWaterWorld(ECS::Entity entity, float worldX, float worldZ, float radius,
                                float targetDepth, float hardness, float amount);
    // 水位图尺寸与是否可涂（属性面板提示用）。
    bool GetWaterMapInfo(ECS::Entity entity, uint32_t& outWidth, uint32_t& outHeight,
                         bool& outPaintable) const;

    // ===== 显式保存：把笔刷产物写出为 PNG（绝对路径，追加在尾部）=====
    // 返回 1 = 已写出（同时清对应脏标记），0 = 没有需要保存的改动（非错误），
    // -1 = 写出失败（errorMessage 给原因）。数据源是 CPU 镜像，与屏幕内容一致。
    int ExportSculptedHeightmap(ECS::Entity entity, const std::string& absolutePath,
                                std::string* errorMessage = nullptr);
    int ExportPaintedControlMap(ECS::Entity entity, const std::string& absolutePath,
                                std::string* errorMessage = nullptr);
    int ExportPaintedGrassMap(ECS::Entity entity, const std::string& absolutePath,
                              std::string* errorMessage = nullptr);
    int ExportPaintedWaterMap(ECS::Entity entity, const std::string& absolutePath,
                              std::string* errorMessage = nullptr);

private:
    static constexpr uint32_t kFramesInFlight = 3;
    // 视图段数（追加常量不影响布局）：段 0 = 场景视图/移动端游戏相机，
    // 段 1 = 编辑器游戏视图相机，段 2 = 反射探针视图（6 面共用该段）。
    // 每段各自持有实例缓冲 / 间接命令 / 描述符集，因此同一帧里三个视图
    // 互不覆盖（这正是「多视口」在同一帧内的实现方式）。
    static constexpr int kGrassCullViewSlots = 3;
    static constexpr int kTerrainMdiViewSlots = 3;

    // 反射探针 = 同一帧里的第三个视图，与「场景视图 / 游戏视图」同级，但**不参与**
    // 「主视图剔除参考系缓存」：
    //   * 不写：探针的 6 个面是 1:1 纵横比的独立相机，把它写进共享参考系会污染
    //     下一帧首个地形/草剔除（实测把主视图地形候选从 98 个 tile 降到 56 个，
    //     并把 terrainMdiTileCapacity 永久锁在错误容量上 ⇒ 主视图地形消失）。
    //   * 不读：探针本来就该用自己面相机的视锥剔除，复用主视图缓存会让背向的
    //     那几面缺地形/草。
    // 凡是通过 Prepare 缓存做剔除的路径（地形 MDI、草）都要按这个判据分叉，
    // 落到「用本视图现场矩阵提平面」的既有分支上。
    static constexpr int kReflectionProbeViewSlot = 2;
    static constexpr bool IsProbeViewSlot(int slot) { return slot >= kReflectionProbeViewSlot; }

    // 反射探针 6 个面在**同一帧、同一个命令缓冲**里逐面顺序录制（一次提交，见
    // RenderSceneProbeCapture 的面循环）。这带来两层分段要求，必须分开看：
    //   * GPU 侧按视图分段的缓冲（地形 MDI 实例/间接命令、草紧凑流）6 面可以
    //     **共用同一段**：每面的剔除 dispatch 在自己的 draw 之前按命令序执行，
    //     执行期读到的永远是本面刚写的数据。
    //   * host 侧用 memcpy 写的相机 UBO **不能**共用：写发生在录制期、与命令序
    //     无关，6 面录完内存里只剩第 6 面的矩阵 ⇒ 6 个面全用第 6 面的相机出图。
    // 因此 UBO / 描述符集必须再按「面」分段（见 Resource::probeUniformBuffers）。
    static constexpr int kProbeFaceCount = 6;

    struct Resource {
        ECS::Entity entity = ECS::INVALID_ENTITY;
        ECS::TerrainComponent settings;
        TerrainChunkManager chunks;

        glm::mat4 model = glm::mat4(1.0f);
        glm::mat4 previousModel = glm::mat4(1.0f);
        bool hasPreviousModel = false;

        std::array<TerrainPatch, 3> patches;
        // MDI tile patch uses the original chunk density divided by the 4x4
        // subdivision, so total terrain sampling density stays unchanged.
        std::array<TerrainPatch, 3> terrainMdiPatches;
        std::array<VulkanBuffer, kFramesInFlight> instanceBuffers;
        // CSM commands are recorded before the main geometry pass. Keep a
        // separate instance stream so the later main-pass upload cannot
        // overwrite the terrain caster data before GPU execution.
        std::array<VulkanBuffer, kFramesInFlight> csmInstanceBuffers;
        std::array<std::unique_ptr<VulkanBuffer>, kFramesInFlight> uniformBuffers;
        std::array<VkDescriptorSet, kFramesInFlight> descriptorSets{};
        // 反射探针（第三个视图）单独一份相机 UBO + 描述符集。必须分流，理由与上面
        // csmInstanceBuffers 同类但更隐蔽：VulkanBuffer::Write 是 host 映射 memcpy
        // （无命令流排序），同一帧里所有地形绘制在执行时读到的是**最后一次**写入的
        // projView。探针在帧尾绘制，若不隔离就会把 90° 面相机矩阵写进共享 UBO，
        // 让主视图已录制的地形 MDI 用探针矩阵执行 —— 表现为整片地形消失。
        // 又因为探针 6 个面在同一命令缓冲里逐面录制，写 UBO 的次数是每帧 6 次，
        // 所以这里再按「面」分段：只有 [frame][face] 各自独立，6 个面才能拿到
        // 各自相机的矩阵。见 kProbeFaceCount 的注释。
        std::array<std::array<std::unique_ptr<VulkanBuffer>, kProbeFaceCount>,
                   kFramesInFlight> probeUniformBuffers;
        std::array<std::array<VkDescriptorSet, kProbeFaceCount>, kFramesInFlight>
            probeDescriptorSets{};
        size_t instanceCapacity = 0;
        size_t csmInstanceCapacity = 0;

        std::string heightmapKey;
        std::array<std::string, 4> layerKeys{};
        std::string controlKey;
        bool useControlMap = false;
        std::vector<std::string> ownedTextureKeys;

        // ===== 编辑器地形笔刷：新增成员一律追加在结构体尾部 =====
        // 本头被大量 TU 包含，而沙箱构建不记录头文件依赖。若把新成员插在中间，
        // 会挪动 layerKeys / controlKey / ownedTextureKeys 的偏移，未重编的 TU
        // 会按旧偏移写坏这些 std::string，表现为关闭期析构崩溃（已实测踩中）。
        //
        // 高度图 CPU 镜像（行主序、顶左原点，与 HeightmapPixels16 同序）。
        // 地形笔刷只改这份镜像再局部上传；渲染快照的 HashTerrain 只哈希路径，
        // 改内容不会触发资源重建，因此笔刷不受 EnsureResource 的相等判定影响。
        std::vector<uint16_t> heightmapCpu;
        uint32_t heightmapWidth = 0;
        uint32_t heightmapHeight = 0;
        // 程序化平坦地形（heightmapPath 为空时生成），属性面板用于提示用户。
        bool heightmapProcedural = false;

        // 控制图 CPU 镜像（RGBA8、行主序、顶左原点），通道 R/G/B/A = 图层 0..3 权重。
        // 材质笔刷只改这份镜像再局部回写，与高度图同一套路，所以同样必须追加在尾部。
        std::vector<uint8_t> controlCpu;
        uint32_t controlWidth = 0;
        uint32_t controlHeight = 0;
        // 控制图是程序化生成的（controlMapPath 为空或文件解码失败）还是来自文件。
        bool controlProcedural = false;

        // 草密度图 CPU 镜像（R8 单通道、行主序、顶左原点，0 = 无草）。
        // 分辨率与高度图一致，笔刷只改镜像再局部回写；散布实例由镜像重建。
        std::string grassKey;
        std::vector<uint8_t> grassCpu;
        uint32_t grassWidth = 0;
        uint32_t grassHeight = 0;
        // 草实例流：脏标志 + 每帧槽位顶点缓冲（与 terrain chunk 实例同模式）。
        bool grassDirty = true;
        std::array<VulkanBuffer, kFramesInFlight> grassInstanceBuffers;
        std::vector<GrassBladeInstance> grassStaging;
        uint32_t grassInstanceCount = 0;
        uint32_t grassInstanceCapacity = 0;
        // 草逐桶视锥剔除（追加在尾部）：桶区间 + 本帧视锥缓存。
        // 视锥随 Prepare 与 chunk 可见性一同写入 Resource（阴影 pass 的
        // 无视锥 Prepare 会覆盖它，但草 CSM 通道用光矩阵现场提取，不读这里）。
        std::vector<GrassChunkBucket> grassBuckets;
        std::array<Plane, 6> grassFrustumPlanes{};
        bool grassUseFrustumCulling = false;
        // 草逐桶距离剔除用的相机位置（随 Prepare 与视锥一同缓存）。
        glm::vec3 grassCameraPosition{0.0f};
        // 视锥粗筛结果缓存：相机/视锥/模型未变化时，草桶不再逐桶重复做
        // AABB + 水平距离测试；GPU 每帧仍正常生成当前帧槽位的命令。
        struct GrassCullCandidate {
            uint32_t first = 0;
            uint32_t count = 0;
            float yLo = 0.0f;
            float yHi = 0.0f;
            float distance = 0.0f;
            float minX = 0.0f;
            float minZ = 0.0f;
            float maxX = 0.0f;
            float maxZ = 0.0f;
        };
        bool grassCullCacheValid = false;
        glm::vec3 grassCullCachedCameraPosition{0.0f};
        glm::mat4 grassCullCachedModel{1.0f};
        std::array<Plane, 6> grassCullCachedFrustumPlanes{};
        std::vector<uint8_t> grassVisibleBucketMask;
        std::vector<GrassCullCandidate> grassCullCandidates;
        bool grassCullCoarseOverflow = false;
        uint32_t grassCullCandidateBlades = 0;
        // ===== 笔刷产物脏标记（追加在尾部）：显式保存时据此写出并清零 =====
        // 高度/控制图被对应笔刷改过即置位；草的 paintedDirty 与 grassDirty
        // （实例流重建标志）语义不同，别混用。
        bool heightmapPaintedDirty = false;
        bool controlPaintedDirty = false;
        bool grassPaintedDirty = false;

        // 水位图 CPU 镜像（R8 单通道、行主序、顶左原点，0 = 无水）。
        // 语义 = 归一化水深（1.0 = kTerrainWaterMaxDepth 米），与高度图同分辨率，
        // 水位笔刷只改镜像再局部回写。渲染侧当前为地形着色器的不透明 mask。
        std::string waterKey;
        std::vector<uint8_t> waterCpu;
        uint32_t waterWidth = 0;
        uint32_t waterHeight = 0;
        bool waterPaintedDirty = false;

        // 水面网格（追加在尾部）：覆盖整块地形的静态 UV 网格（无实例），
        // VS 按 高度图+水位图 抬升顶点；水地图/高度图是纹理，涂水无需重建网格。
        // 复用 TerrainPatch 的构建/清理（其顶点本来就是 vec2 UV）。
        TerrainPatch waterPatch;

        // ===== 草地 GPU 逐桶剔除（追加在尾部）=====
        // 世界空间桶 SSBO：minFirst = (AABB min.xyz, firstInstance)，
        // maxCount = (AABB max.xyz, instanceCount)，由 CPU 变换后写入。
        std::array<VulkanBuffer, kFramesInFlight> grassBucketGpuBuffers;
        // 间接命令 SSBO：[viewSlot][bucket] 每命令 4 个 uint（vertexCount/
        // instanceCount/firstVertex/firstInstance = 16B，与 sizeof(VkDrawIndirectCommand)
        // 一致），剔除 compute 写、主 pass vkCmdDrawIndirect 读。
        std::array<VulkanBuffer, kFramesInFlight> grassIndirectBuffers;
        size_t grassGpuBucketCapacity = 0;
        uint32_t grassGpuBucketTotal = 0;
        // 桶数据/模型矩阵上传脏标记：FinalizeGrassBuckets 与模型变化时重传。
        bool grassGpuBucketDirty = true;
        glm::mat4 grassGpuBucketUploadedModel = glm::mat4(1.0f);
        std::array<VkDescriptorSet, kFramesInFlight> grassCullDescriptorSets{};
        // 每帧每视图槽的 dispatch 记录：RenderGrass 按 projView 位级匹配选段，
        // 未命中（未 dispatch / CSM 不可用 / 编辑器关闭剔除）走 CPU 逐桶回退。
        struct GrassGpuCullView {
            bool dispatched = false;
            glm::mat4 viewProj = glm::mat4(1.0f);
        };
        std::array<std::array<GrassGpuCullView, 2>, kFramesInFlight> grassGpuCullViews{};
        uint32_t grassGpuCullClearedFrame = 0xFFFFFFFFu;

        // ===== 草地叶片级 GPU 剔除（追加在尾部）=====
        // 紧凑实例流：compute atomicAdd 把可见叶写进来（[viewSlot 段][叶] 连续），
        // 主 pass 以 instance-rate 顶点属性绑定绘制。DEVICE_LOCAL（GPU 写 GPU 读）。
        std::array<VulkanBuffer, kFramesInFlight> grassBladeCompactBuffers;
        // 间接命令 + 原子计数：每视图槽 16B = VkDrawIndirectCommand，其
        // instanceCount 字段即 compute 的 atomicAdd 计数器（每帧 fill 归零）。
        // host-visible：调试读回上一周期计数。
        std::array<VulkanBuffer, kFramesInFlight> grassBladeCmdBuffers;
        // 剔除参数 SSBO（272B）：model + 6 平面 + camAndDist + heightRange
        // + Hi-Z 投影/参数；按 [viewSlot] 分段。
        std::array<VulkanBuffer, kFramesInFlight> grassBladeParamsBuffers;
        // 每个视图使用独立 descriptor set，避免 SceneView/GameView 在同一帧
        // 更新 binding 5 时互相覆盖 Hi-Z image。
        std::array<std::array<VkDescriptorSet, kGrassCullViewSlots>, kFramesInFlight>
            grassBladeCullDescriptorSets{};
        size_t grassBladeCompactCapacity = 0;   // 每视图槽位容量（叶数）
        // 帧内视图槽分配：同帧第 1/2 次叶片级 dispatch 各占 slot 0/1，跨帧
        // 复用槽位间隔 ≥3 帧，上次该槽的 draw 早已完成。
        std::array<uint32_t, kFramesInFlight> grassBladeSlotCounters{};
        // 全局 Y 范围（FinalizeGrassBuckets 扫高度镜像时顺带累出）：叶级保守
        // AABB 的 Y 区间，随雕刻自动更新。
        float grassBladeMinY = 0.0f;
        float grassBladeMaxY = 0.0f;
        // 命令 SSBO 每帧 fill 清零标记（帧号）：两个视图段共用一张命令缓冲，
        // fill 每帧每资源只录一次——逐视图 fill 会把先 dispatch 的视图段计数抹零。
        uint32_t grassBladeCmdResetFrame = 0xFFFFFFFFu;
        // CPU 粗筛幸存桶列表（叶片级剔除两级化的第二级输入）：每项 16B
        // {firstInstance, count, yLo, yHi}，录制期 vkCmdUpdateBuffer 一次性写入，
        // dispatch 一工作组一桶。host-visible 便于调试读回。
        std::array<VulkanBuffer, kFramesInFlight> grassBladeBucketListBuffers;

        // ===== 地形 GPU MDI（每个原有 chunk 固定细分为 4x4 tile）=====
        // 实例缓冲按 [viewSlot][tile] 分段，避免同一帧场景视图与 GameView
        // 的 CPU 上传互相覆盖。GPU 会把可见实例压缩到 compact buffer，
        // 每个 viewSlot/LOD 只保留一条 indirect draw。
        std::array<VulkanBuffer, kFramesInFlight> terrainMdiInstanceBuffers;
        std::array<VulkanBuffer, kFramesInFlight> terrainMdiCompactInstanceBuffers;
        std::array<VulkanBuffer, kFramesInFlight> terrainMdiTileBuffers;
        std::array<VulkanBuffer, kFramesInFlight> terrainMdiIndirectBuffers;
        // 每个视图槽一条 Hi-Z 投影矩阵/参数记录；用 host-visible buffer，
        // 录制多个视图时不会因为最后一次 CPU 写入覆盖前一个 dispatch。
        std::array<VulkanBuffer, kFramesInFlight> terrainMdiCullViewBuffers;
        std::array<VkDescriptorSet, kFramesInFlight> terrainMdiCullDescriptorSets{};
        std::vector<TerrainChunkInstance> terrainMdiInstances;
        std::vector<TerrainMdiTile> terrainMdiTiles;
        std::vector<AABB> terrainMdiLocalBounds;
        uint32_t terrainMdiGridCount = 0;
        // 当前 RecordTerrainGpuCull 调用生成的 CPU 粗筛候选数。
        uint32_t terrainMdiTileCount = 0;
        // 当前共享候选流按 LOD 压缩后的命令数；三者之和等于
        // terrainMdiTileCount，每个 tile 只对应一个 indirect command。
        std::array<uint32_t, 3> terrainMdiCandidateLodCounts{};
        // 每个视图槽独立保存候选数：CPU 粗筛后两个视图可能得到不同数量，
        // 但实例/间接命令缓冲仍按固定 capacity 分段，避免 slot 1 覆盖 slot 0。
        std::array<uint32_t, kTerrainMdiViewSlots> terrainMdiTileCounts{};
        std::array<std::array<uint32_t, 3>, kTerrainMdiViewSlots>
            terrainMdiLodTileCounts{};
        // Hi-Z 关闭时复用 CPU 候选流做 3 条 MDI draw；记录每个 LOD 在
        // [viewSlot][tile] 源实例段内的起始偏移。
        std::array<std::array<uint32_t, 3>, kTerrainMdiViewSlots>
            terrainMdiLodInstanceOffsets{};
        size_t terrainMdiTileCapacity = 0;
        bool terrainMdiBoundsDirty = true;
        // 地形 MDI CPU 粗筛结果缓存。tile bounds 或 culling reference 变化
        // 时失效；命中时复用 terrainMdiTiles/terrainMdiInstances，仍按当前
        // 帧槽上传并执行 GPU 细筛。
        bool terrainMdiCullCacheValid = false;
        glm::vec3 terrainMdiCachedCameraPosition{0.0f};
        glm::mat4 terrainMdiCachedModel{1.0f};
        std::array<Plane, 6> terrainMdiCachedFrustumPlanes{};
        // 与草地剔除相同：Prepare 阶段缓存 CPU 粗筛使用的参考系。阴影 Prepare
        // 不带视锥时不得覆盖这份主几何参考系。
        std::array<Plane, 6> terrainMdiFrustumPlanes{};
        bool terrainMdiUseFrustumCulling = false;
        glm::vec3 terrainMdiCameraPosition{0.0f};
        struct TerrainMdiCullView {
            bool dispatched = false;
            bool usesCompactInstances = false;
            bool usesCachedCull = false;
            glm::mat4 viewProj = glm::mat4(1.0f);
        };
        std::array<std::array<TerrainMdiCullView, kTerrainMdiViewSlots>, kFramesInFlight>
            terrainMdiCullViews{};
        uint32_t terrainMdiCullClearedFrame = 0xFFFFFFFFu;

        // ===== 反射探针视图的 CPU chunk 回退实例流（追加在结构体尾部）=====
        // 探针刻意不走地形 MDI（面相机远平面大 → 单面就要 300~434 个 tile，会把
        // terrainMdi 缓冲在帧内撑爆重建，见 RenderSceneProbeCapture 面循环注释），
        // 因此它走非 MDI 的 chunk 绘制路径，而那条路径用 VulkanBuffer::Write
        // （host memcpy，无命令流排序）写实例流 —— 与相机 UBO 完全同一个坑：
        // 若与主视图共用一份，探针在帧尾的写入会盖掉主视图已录制的实例数据。
        // 因此单独一份缓冲 + 独立容量，主视图那条路径行为不变。
        // Probe faces are recorded into one command buffer.  These are host-written
        // instance streams, so a single buffer per frame would be overwritten by the
        // last recorded face before the GPU executes the first face.
        std::array<std::array<VulkanBuffer, kProbeFaceCount>, kFramesInFlight>
            probeInstanceBuffers;
        size_t probeInstanceCapacity = 0;
    };

    void CollectTerrainEntities(ECS::Entity entity, std::vector<ECS::Entity>& entities) const;
    Resource* EnsureResource(ECS::Entity entity, const ECS::TerrainComponent& settings);
    std::unique_ptr<Resource> CreateResource(ECS::Entity entity, const ECS::TerrainComponent& settings);
    void DestroyResource(Resource& resource);
    bool SettingsEqual(const ECS::TerrainComponent& lhs, const ECS::TerrainComponent& rhs) const;

    bool EnsureWhiteFallback();
    bool CreateDescriptorResources();
    void DestroyDescriptorResources();
    bool CreatePipelines();
    bool BuildPatch(TerrainPatch& patch, uint32_t resolution);
    bool CreateInstanceBuffers(Resource& resource, size_t capacity, bool probeView = false);
    bool EnsureCsmInstanceCapacity(Resource& resource, size_t visibleCount);
    bool CreateUniformBuffers(Resource& resource);
    bool CreateDescriptorSets(Resource& resource);
    bool EnsureInstanceCapacity(Resource& resource, size_t visibleCount, bool probeView = false);

    // applyTAAJitter=false 供 CSM 深度通道用：光空间投影不吃主相机抖动。
    // viewSlot / probeFace：反射探针（2）写自己那一份 [面] UBO，其余视图共用原
    // UBO（行为不变）。探针 6 面在同一命令缓冲里顺序录制，host memcpy 无命令序，
    // 所以必须按面分段 —— 否则 6 面全都拿到最后一个面的矩阵。
    void UpdateUniform(Resource& resource,
                       const glm::mat4& projView,
                       const glm::mat4& prevProjView,
                       const glm::vec3& cameraPosition,
                       bool applyTAAJitter = true,
                       int viewSlot = 0,
                       int probeFace = 0);
    // 按 viewSlot / probeFace 选出本视图要绑的相机 UBO 描述符集（探针面集未建立
    // 时回退主集，保证首帧/资源重建期不绑 VK_NULL_HANDLE）。
    VkDescriptorSet ViewDescriptorSet(const Resource& resource, uint32_t frame,
                                      int viewSlot, int probeFace) const;
    // 按草密度图 CPU 镜像重新散布草叶实例（写 grassStaging，标记待上传），
    // 结束时调用 FinalizeGrassBuckets 重建逐桶区间。
    void RebuildGrassInstances(Resource& resource);
    // 把 grassStaging 按 chunk 网格稳定分桶：重排实例流、记录每桶区间与局部包围盒。
    void FinalizeGrassBuckets(Resource& resource);
    bool EnsureGrassCullCache(Resource& resource,
                              const glm::vec3& cameraPosition,
                              const std::array<Plane, 6>& frustumPlanes);
    // 确保草实例缓冲已按 grassStaging 上传（主 pass 与阴影 pass 共用），
    // 返回可绘制的实例数（0 = 无草可画）。
    uint32_t EnsureGrassInstancesUploaded(Resource& resource, uint32_t frame);
    // 逐桶视锥 + 距离测试并绘制已绑定的草实例流（管线/descriptor/顶点缓冲由调用方绑定）。
    // statsTag 用于 MIKAN_GRASS_CULL_STATS=1 时的剔除统计日志（区分主 pass / CSM 级联）。
    // extraFrustum：可选的第二个视锥门（草影路径传主相机视锥；主 pass 不传）。
    void RenderGrassBuckets(VkCommandBuffer commandBuffer, Resource& resource,
                            const std::array<Plane, 6>& frustumPlanes,
                            bool useFrustumCulling, const glm::vec3& cameraPosition,
                            const char* statsTag = "main",
                            const std::array<Plane, 6>* extraFrustum = nullptr);
    // ===== 草地 GPU 逐桶剔除（私有辅助，追加在尾部）=====
    bool EnsureGrassCullPipeline();
    bool EnsureGrassCullBuffers(Resource& resource, uint32_t frame);
    void UploadGrassCullBuckets(VkCommandBuffer commandBuffer, Resource& resource, uint32_t frame);
    void CleanupGrassCullResources();
    // ===== 草地叶片级 GPU 剔除（追加在尾部）：compute 逐叶测试 + atomicAdd 压缩
    // 实例流 + 间接绘制 instanceCount = 原子计数。粒度 = 单叶（阶段三）。
    bool EnsureGrassBladeCullPipeline();
    bool EnsureGrassBladeCullBuffers(Resource& resource, uint32_t frame, uint32_t instanceCount);
    bool EnsureTerrainMdiCullPipeline();
    bool EnsureTerrainMdiBuffers(Resource& resource, uint32_t frame);
    bool BuildTerrainMdiTiles(Resource& resource, const glm::vec3& cameraPosition,
                              const std::array<Plane, 6>& frustumPlanes,
                              int viewSlot = 0);
    int FindTerrainMdiView(const Resource& resource, uint32_t frame,
                           const glm::mat4& projView) const;
    void CleanupTerrainMdiCullResources();
    // 主 pass 地形之后绘制草（草地管线未就绪或无实例时静默跳过）。
    // projView 用于匹配 GPU 剔除命令段（位级比较），未命中走 CPU 逐桶回退。
    // viewSlot / probeFace：决定绑哪一份相机 UBO 描述符集（草顶点着色器读 UBO 的
    // projView，探针面若绑到主集就会用主相机矩阵把草画到视锥外）。
    void RenderGrass(VkCommandBuffer commandBuffer, Resource& resource, uint32_t frame,
                     const glm::mat4& projView,
                     int viewSlot = 0,
                     int probeFace = 0);
    void RenderInternal(VkCommandBuffer commandBuffer, int width, int height,
                        const glm::mat4& projView,
                        const glm::mat4& prevProjView,
                        const glm::vec3& cameraPosition,
                        bool depthOnly,
                        int viewSlot = 0,
                        int probeFace = 0);

    static std::string MakeTextureKey(ECS::Entity entity, const char* slot);
    static const std::string& GetLayerPath(const ECS::TerrainComponent& settings, int layer);

    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    VulkanPipeline m_Pipeline;
    VulkanPipeline m_GrassPipeline;
    VulkanPipeline m_GrassDepthPipeline;
    VkRenderPass m_GrassCsmRenderPass = VK_NULL_HANDLE;
    VulkanPipeline m_WireframePipeline;
    VulkanPipeline m_DepthPipeline;
    VulkanPipeline m_CsmDepthPipeline;
    VkRenderPass m_CsmRenderPass = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_DescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    bool m_OwnedWhiteFallback = false;

    std::unordered_map<ECS::Entity, std::unique_ptr<Resource>> m_Resources;
    std::vector<Resource*> m_PreparedResources;
    size_t m_VisibleChunkCount = 0;
    bool m_PrimitiveRestartSupported = false;
    // 地形水位图水面网格管线（追加在类成员尾部）：原写 G-buffer，2026-09-19
    // 起 deferred water compositing——改画进共享 WaterTargetRT（尾插新成员，
    // 旧 m_WaterPipeline 移除；布局改动只影响 Engine 内部，Game 不直接使用）。
    VulkanPipeline m_WaterTargetPipeline;
    VkRenderPass m_WaterTargetRenderPass = VK_NULL_HANDLE;

    // ===== 草地 GPU 逐桶剔除（追加在类成员尾部）=====
    VkPipeline m_GrassCullPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_GrassCullPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_GrassCullDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_GrassCullDescriptorPool = VK_NULL_HANDLE;
    // MIKAN_GRASS_GPU_CULL=0 时禁用 GPU 剔除（回退 CPU 逐桶绘制），用于 A/B 验证。
    bool m_GrassGpuCullDisabled = false;

    // ===== 草地叶片级 GPU 剔除（追加在类成员尾部）=====
    VkPipeline m_GrassBladeCullPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_GrassBladeCullPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_GrassBladeCullDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_GrassBladeCullDescriptorPool = VK_NULL_HANDLE;
    // 叶片级复用 m_GrassCullDescriptorPool（SSBO 描述符，池容量已留余量）。
    // MIKAN_GRASS_COMPUTE=1 时启用叶片级剔除（替代桶级 compute 路径）。
    bool m_GrassBladeCullEnabled = false;

    // ===== 地形 GPU MDI（追加在类成员尾部）=====
    VkPipeline m_TerrainMdiCullPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_TerrainMdiCullPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_TerrainMdiCullDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_TerrainMdiCullDescriptorPool = VK_NULL_HANDLE;
    bool m_TerrainMdiCullDisabled = false;
    bool m_TerrainMdiSupported = false;
    uint32_t m_TerrainMdiMaxDrawCount = 0;

    // 地形 MDI 的 Hi-Z 消费已关闭，保留这些字段仅兼容旧资源布局/回退状态。
    glm::mat4 m_TerrainHiZPreviousGameViewProj = glm::mat4(1.0f);
    bool m_TerrainHiZHasPreviousGameView = false;

    // 草地每个视图各自使用上一帧的投影矩阵与深度金字塔：slot 0 = SceneView
    // / 移动端游戏，slot 1 = 编辑器 GameView。
    std::array<glm::mat4, kGrassCullViewSlots> m_GrassHiZPreviousViewProj{};
    std::array<bool, kGrassCullViewSlots> m_GrassHiZHasPreviousView{};
};
