#pragma once
// Renderer2D.h - 2D 渲染核心（UI 与 2D 玩法共用的图元合批渲染器）
// 设计：所有图元(四边形)收集后按纹理分组合批，纹理切换才 flush，
// 单帧以 1-N 个 draw call 提交；顶点由 CPU 完成节点树变换，shader 只做投影+采样。
// ===== 坐标系约定（与 Canvas2D 一致,勿误用数学 y）=====
// DrawRect/DrawSprite/DrawQuad 的坐标 = 画布坐标:左下原点, y 向下(顶部=0)。
// 原因: 本渲染器用 glm::ortho(GL 约定 y 向上) 但未做 Vulkan NDC y 翻转(proj[1][1]*=-1),
// 实际显示 y 向下。因此"屏幕上方"= y 较小;传入数学 y(向上)会视觉颠倒。
#include "Platform/Export.h"
#include "RendererBase.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <vector>
#include <string>
#include <unordered_map>

class TexturePool;
enum class SamplerType;

// 顶点格式（pos2 + uv2 + color4 + screenPxRange1，36B）
struct MIKAN_API Vertex2D {
    glm::vec2 pos;
    glm::vec2 uv;
    glm::vec4 color;
    float screenPxRange = 0.0f;  // 距离场过渡带屏幕像素（仅 shaderMode=1 用；位图模式忽略=0）
};

// 单个 2D 图元：四边形（世界/屏幕坐标，已由节点树变换到最终位置，逆时针）
struct MIKAN_API Quad2D {
    glm::vec2 p0, p1, p2, p3;   // 左下/右下/右上/左上
    glm::vec2 uv0, uv1;         // 源 UV（左下/右上）
    glm::vec4 color = glm::vec4(1.0f);
    VkDescriptorSet texture = VK_NULL_HANDLE;  // 纹理描述符（Renderer2D 的布局）
    int layer = 0;              // 渲染层（整数；数字越大越在上层）
    int shaderMode = 0;         // 0=位图/默认, 1=SDF（距离场，用 ui2d_sdf.frag 管线）
    float screenPxRange = 0.0f; // 距离场过渡带屏幕像素（shaderMode=1；SDF≈2.67×缩放, MSDF=2×缩放）
};

// shaderMode 常量（inline constexpr：编译期常量，C++17 单一定义，无需 dllimport——
// 带 MIKAN_API(dllimport) 的 constexpr 定义会触发 MSVC C2491）
inline constexpr int SHADER_MODE_BITMAP = 0;
inline constexpr int SHADER_MODE_SDF = 1;

// 三角形图元：3 个显式顶点，各自携带 UV。quad 的 uv0/uv1 是"角点对"插值模型，
// 无法表达任意三角形 UV；tight sprite mesh 必须逐顶点 UV，故单独立类型。
// 注意：uv0/uv1/uv2 是【最终采样 UV】（v=1 对应原图顶部，与 Flush 的 v 翻转约定一致），
// 不再经过 quad 那套"上下边对调"发射逻辑。
struct MIKAN_API Tri2D {
    glm::vec2 p0, p1, p2;
    glm::vec2 uv0, uv1, uv2;
    glm::vec4 color = glm::vec4(1.0f);
    VkDescriptorSet texture = VK_NULL_HANDLE;
    int layer = 0;
    int shaderMode = 0;
    float screenPxRange = 0.0f;
};

// Sprite 轮廓网格（CPU 一次性生成，按名字缓存；overdraw 优化）。
// positions 归一化到 [0,1]^2：(0,0)=精灵左上角（画布 y 向下约定）、(1,1)=右下，
// 与 DrawSpriteMesh(pos,size) 的映射 pos+position*size 一致；
// uvs 为最终采样 UV（v=1=原图顶部），与 positions 一一对应；
// coverage = 网格面积/包围盒面积，越低相对整块 quad 省下的填充越多。
struct MIKAN_API SpriteMesh2D {
    std::vector<glm::vec2> positions;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;   // 三角形索引（3 的倍数）
    uint32_t triangleCount = 0;
    float coverage = 1.0f;
};

class MIKAN_API Renderer2D {
public:
    struct RenderViewContext {
        glm::mat4 view = glm::mat4(1.0f);
        glm::mat4 projection = glm::mat4(1.0f);
        glm::vec3 cameraPosition = glm::vec3(0.0f);
        bool has3DCamera = false;
        bool isSceneView = false;
    };

    static Renderer2D& GetInstance();

    bool Init(VkRenderPass offscreenPass, VkRenderPass overlayPass, VkRenderPass displayUIPass, VkRenderPass swapchainUIPass);
    void Cleanup();

    // ===== 每帧流程 =====
    // viewProj: 正交投影矩阵; overlay=false 用离屏管线(世界层,后处理前), true 用主窗口管线(UI 层,后处理后)
    void BeginFrame(VkCommandBuffer cmd, const glm::mat4& viewProj, uint32_t width, uint32_t height, bool overlay);
    // 每帧 2D 渲染开始前调用：重置顶点写入位置。
    // 关键：一帧内可能多次 BeginFrame/Flush（世界层、UI 层、文本等），
    // 顶点缓冲必须按周期递增写入，否则后写的 Flush 会覆盖先前 draw 引用的顶点数据
    // （Vulkan 命令缓冲统一提交后，所有 draw 读取的是缓冲最终状态）。
    void ResetFrame();
    void DrawQuad(const Quad2D& quad);
    // 便捷：纯色矩形 / 带纹理精灵（pos 为左下角, size 为宽高）
    void DrawRect(glm::vec2 pos, glm::vec2 size, const glm::vec4& color, int layer = 0);
    void DrawSprite(glm::vec2 pos, glm::vec2 size, VkDescriptorSet texture,
                    const glm::vec2& uv0 = glm::vec2(0.0f), const glm::vec2& uv1 = glm::vec2(1.0f),
                    const glm::vec4& color = glm::vec4(1.0f), int layer = 0);
    // 九宫格精灵：border=(左,右,上,下) 源纹理像素边框，srcSize=源纹理像素尺寸
    void DrawSlice9(glm::vec2 pos, glm::vec2 size, VkDescriptorSet texture,
                    const glm::vec4& border, const glm::vec2& srcSize,
                    const glm::vec2& uv0 = glm::vec2(0.0f), const glm::vec2& uv1 = glm::vec2(1.0f),
                    const glm::vec4& color = glm::vec4(1.0f), int layer = 0);
    // ===== 线/圆图元（同样走 quad 合批；纯色，白色纹理）=====
    // 线段 a→b：旋转细长矩形实现，width 为线宽（画布像素）
    void DrawLine(glm::vec2 a, glm::vec2 b, float width, const glm::vec4& color, int layer = 0);
    // 实心圆：扇形逐段展开为退化 quad（每段 1 quad），segments 越多边缘越圆
    void DrawCircle(glm::vec2 center, float radius, const glm::vec4& color,
                    int layer = 0, int segments = 40);
    // 空心圆环：thickness 为环宽（画布像素），适合轮廓/瞄准圈/调试
    void DrawRing(glm::vec2 center, float radius, float thickness, const glm::vec4& color,
                  int layer = 0, int segments = 40);
    // 任意三角形（tight mesh 的基础原语；同样走合批，1 tri = 3 顶点）
    void DrawTriangle(const Tri2D& tri);
    // Sprite 轮廓网格绘制：用按 alpha 轮廓三角化的 tight mesh 替代整块 quad，
    // 透明区域不再产生 overdraw。meshName 同时作为纹理名（GetTexture 语义）；
    // 网格未生成时回退为整块 DrawSprite。
    void DrawSpriteMesh(glm::vec2 pos, glm::vec2 size, const std::string& meshName,
                        const glm::vec4& color = glm::vec4(1.0f), int layer = 0,
                        int shaderMode = 0, float screenPxRange = 0.0f);
    // ===== Sprite 轮廓网格生成（alpha 阈值 → marching squares → RDP 简化 → 耳切三角化）=====
    // 从 RGBA8 像素缓冲生成（行 0 = 图像顶部；纯 CPU，无需 Vulkan）。
    // 成功后按 name 缓存；alphaThreshold ∈ (0,1]，alpha/255 ≥ 阈值视为不透明。
    bool GenerateSpriteMeshFromPixels(const std::string& name, const uint8_t* rgba, int w, int h,
                                      float alphaThreshold = 0.5f);
    // 从已加载纹理的源文件重解码生成（SDL_image，CPU 一次性；需先 LoadTexture 记录源路径）
    bool GenerateSpriteMesh(const std::string& name, float alphaThreshold = 0.5f);
    const SpriteMesh2D* GetSpriteMesh(const std::string& name) const;
    // 当前已收集未 Flush 的图元数（调试/测试用）
    uint32_t GetPendingQuadCount() const { return (uint32_t)m_Quads.size(); }
    uint32_t GetPendingTriangleCount() const { return (uint32_t)m_Tris.size(); }
    // 提交全部图元（纹理分组合批）
    void Flush();
    // 场景视图 pass 隔离：切换到第二顶点缓冲（与游戏视图共享缓冲会互相覆盖）
    void UseSecondaryBuffer(bool use) { m_UseSecondaryBuffer = use; }
    // UI 叠加模式：链末 tonemap 之后画 UI，alpha 混合叠加在离屏结果之上
    // SetDisplayUI：编辑器路径（显示附件 loadOp=LOAD pass）；SetSwapchainUI：游戏模式（swapchain loadOp=LOAD pass）
    void SetDisplayUI(bool use) { m_IsDisplayUI = use; }
    void SetSwapchainUI(bool use) { m_IsSwapchainUI = use; }

    // 当前 UI pass 对应的 3D 视口相机；游戏模块可用它把世界锚点投影到
    // 本次 SceneView/GameView 的屏幕坐标，而不必改变 IGameModule ABI。
    void SetRenderViewContext(const glm::mat4* view, const glm::mat4* projection,
                              bool isSceneView) {
        m_RenderViewContext = RenderViewContext{};
        if (view != nullptr && projection != nullptr) {
            m_RenderViewContext.view = *view;
            m_RenderViewContext.projection = *projection;
            m_RenderViewContext.cameraPosition = glm::vec3(glm::inverse(*view)[3]);
            m_RenderViewContext.has3DCamera = true;
            m_RenderViewContext.isSceneView = isSceneView;
        }
    }
    const RenderViewContext& GetRenderViewContext() const { return m_RenderViewContext; }

    // ===== 纹理管理（2D 专用描述符：从 TexturePool 取图像，用本渲染器布局建描述符）=====
    VkDescriptorSetLayout GetTextureLayout() const { return m_TextureLayout; }
    bool LoadTexture(const std::string& name, const std::string& filePath);
    bool LoadTexture(const std::string& name, const std::string& filePath, SamplerType sampler);
    VkDescriptorSet GetTexture(const std::string& name) const;  // 未找到返回白色
    VkDescriptorSet GetWhiteTexture() const { return m_WhiteDescriptor; }

private:
    Renderer2D() = default;
    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    bool InitDeviceResources();
    bool InitPipelines(VkRenderPass offscreenPass, VkRenderPass overlayPass,
                       VkRenderPass displayUIPass, VkRenderPass swapchainUIPass);
    void CleanupPipelines();
    bool CreateWhiteTexture();
    void EnsureVertexCapacity(size_t quads);

    // 描述符
    VkDescriptorSetLayout m_TextureLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_TexturePoolHandle = VK_NULL_HANDLE;   // 2D 专用池
    VkDescriptorSet m_WhiteDescriptor = VK_NULL_HANDLE;
    VkImage m_WhiteImage = VK_NULL_HANDLE;
    VkDeviceMemory m_WhiteMemory = VK_NULL_HANDLE;
    VkImageView m_WhiteView = VK_NULL_HANDLE;
    VkSampler m_WhiteSampler = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkDescriptorSet> m_Textures;

    // 渲染
    VulkanPipeline m_Pipeline;          // 离屏(世界层, 后处理前) 位图
    VulkanPipeline m_OverlayPipeline;   // 主窗口(UI 层, 后处理后) 位图
    VulkanPipeline m_SdfPipeline;       // 离屏 SDF（距离场 shader）
    VulkanPipeline m_SdfOverlayPipeline;// 主窗口 SDF
    VulkanPipeline m_DisplayUIPipeline; // UI 叠加（链末 tonemap 后，显示附件 loadOp=LOAD pass）位图
    VulkanPipeline m_DisplayUISdfPipeline; // UI 叠加 SDF
    VulkanPipeline m_SwapchainUIPipeline; // UI 叠加（swapchain loadOp=LOAD pass，游戏模式）位图
    VulkanPipeline m_SwapchainUISdfPipeline; // UI 叠加 swapchain SDF
    VulkanPipeline* m_CurrentPipeline = nullptr;
    // 三重缓冲：每个交换链帧使用独立的 host-visible 顶点缓冲，避免 CPU
    // 重写本帧 UI 时覆盖 GPU 仍在读取的上一/上上帧文字和图元。
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
    VulkanBuffer m_VertexBuffers[MAX_FRAMES_IN_FLIGHT];
    VulkanBuffer m_VertexBuffersSecondary[MAX_FRAMES_IN_FLIGHT];   // SceneView 与 GameView 的 pass 隔离
    bool m_UseSecondaryBuffer = false;
    uint32_t m_CurrentFrameIndex = 0;
    VkRenderPass m_OffscreenPass = VK_NULL_HANDLE;
    VkRenderPass m_OverlayPass = VK_NULL_HANDLE;
    VkRenderPass m_DisplayUIPass = VK_NULL_HANDLE;
    VkRenderPass m_SwapchainUIPass = VK_NULL_HANDLE;
    uint32_t m_MaxVertices = 0;
    uint32_t m_VertexWriteOffset = 0;  // 帧内累计已写入顶点数（跨 BeginFrame/Flush 周期递增，ResetFrame 清零）
    VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
    bool m_IsOverlay = false;           // 当前周期用 overlay(主窗口) 管线还是离屏管线
    bool m_IsDisplayUI = false;         // 当前周期用 UI 叠加管线（编辑器：显示附件 pass）
    bool m_IsSwapchainUI = false;       // 当前周期用 UI 叠加管线（游戏模式：swapchain pass）
    glm::mat4 m_ViewProj = glm::mat4(1.0f);
    RenderViewContext m_RenderViewContext;
    std::vector<Quad2D> m_Quads;   // 每帧收集
    std::vector<Tri2D> m_Tris;     // 每帧收集（Flush 与 m_Quads 合流排序，保持层序）
    // Sprite 轮廓网格缓存（GenerateSpriteMesh* 生成）与纹理源路径记录
    std::unordered_map<std::string, SpriteMesh2D> m_SpriteMeshes;
    std::unordered_map<std::string, std::string> m_TexturePaths;

    static constexpr uint32_t MAX_QUADS = 16384;      // 单帧图元上限（64K 顶点）
    static constexpr uint32_t MAX_2D_TEXTURES = 256;  // 2D 纹理描述符上限
};
