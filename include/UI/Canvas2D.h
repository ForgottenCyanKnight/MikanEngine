#pragma once
// Canvas2D.h - 2D 场景图（UI 与 2D 玩法深度结合的统一画布）
// ===== 坐标系权威约定（所有 2D 代码必须遵守,防止歧义）=====
// 1. 画布/屏幕坐标: 左下原点, y 向下(顶部=0)。即 y 值越大越靠屏幕下方。
//    这是 Sprite2D/Text/Button 等 ECS 实体的 position 语义,也是实际显示方向。
// 2. 原因: Renderer2D 用 glm::ortho(GL 约定 y 向上),但 Vulkan NDC 的 y 与 GL 相反,
//    且 2D 矩阵未像 3D 那样做 proj[1][1]*=-1 → 实际显示 y 向下,恰好符合画布约定。
//    因此: 屏幕上方 = y 较小; 若把数学 y(向上)直接传入,视觉会上下颠倒。
// 3. 鼠标/输入(屏幕像素 y 向下)与画布坐标同向,直接可用。
// ============================================================
// 设计：2D 严格由全局 ECS 场景树管理（Unity Canvas 模型）——
//   Canvas 实体(canvas2d 组件) 作为父节点, UI 实体(sprite2d/textComp/button/slice9, isUI=true) 为其子级;
//   isUI=false → 世界坐标（玩法精灵，用世界正交相机）; isUI=true → 屏幕坐标（UI 控件）。
//   渲染/交互均从 ECS 收集（RenderECSNodes/UpdateCanvasNodeRecursive），无独立子树。
#include "Platform/Export.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/RenderWorld.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <functional>

namespace UI {

// ===== 画布：ECS 场景树驱动 + 双相机 + 渲染/交互 =====
class MIKAN_API Canvas2D {
public:
    static Canvas2D& GetInstance();

    void SetViewport(uint32_t width, uint32_t height);
    void SetWorldCamera(glm::vec2 center, float zoom = 1.0f);  // 玩法层正交相机
    // 从场景树读取 Camera2DComponent 驱动世界层相机(无组件则默认 (0,0,1))。每帧调用。
    void SyncCameraFromScene();
    // 视图矩阵公开(供游戏世界/UI 层渲染在 Canvas 之外追加内容,与场景树 2D 实体同相机)
    const glm::mat4& GetWorldViewProj() const { return m_WorldViewProj; }
    const glm::mat4& GetUIViewProj() const { return m_UIViewProj; }

    void Clear();

    // 每帧：更新按钮状态/命中（鼠标为屏幕像素，y 向下 → 内部翻转）
    void Update(glm::vec2 mouseScreenPos, bool mouseDown);
    // 渲染：世界层(离屏 target,后处理前)与 UI 层(主窗口,后处理后)分两处调用——
    // 2D 玩法与 3D 共用后处理, UI 不受 bloom 等影响
    void RenderWorld(Renderer2D& r2d, VkCommandBuffer cmd, const ::RenderWorld& world);
    void RenderUI(Renderer2D& r2d, VkCommandBuffer cmd, const ::RenderWorld& world);

    // 锚点拉伸布局计算：(parentSize, anchorMin, anchorMax, posOffset, size) → (absPos, size)
    // position 语义 = 相对锚点(anchorMin)的像素偏移；outPos = parentSize*anchorMin + posOffset
    // 编辑器 gizmo/停靠面板也复用此函数，保证与渲染结果一致
    static void ComputeAnchorLayout(const glm::vec2& parentSize,
                                    const glm::vec2& anchorMin, const glm::vec2& anchorMax,
                                    const glm::vec2& posOffset, const glm::vec2& size,
                                    glm::vec2& outPos, glm::vec2& outSize);

private:
    Canvas2D() = default;
    Canvas2D(const Canvas2D&) = delete;
    Canvas2D& operator=(const Canvas2D&) = delete;

    void RenderECSNodes(Renderer2D& r2d, const ::RenderWorld& world, bool uiPass);   // 从 RenderWorld 快照渲染
    void RenderCanvasChildren(Renderer2D& r2d, const ::RenderWorld& world, ECS::Entity canvas);  // 画布的子级 UI 实体(递归)
    void RenderCanvasNodeRecursive(Renderer2D& r2d, const ::RenderWorld& world, ECS::Entity entity, glm::vec2 parentPos, glm::vec2 parentSize, int depth);  // 递归渲染 UI 节点树(带锚点布局+深度防御)
    void UpdateCanvasNodeRecursive(ECS::Entity entity, glm::vec2 parentPos, glm::vec2 mousePos, bool mouseDown, bool clickEdge, int depth);  // 递归按钮命中(带深度防御)
    void RenderSpriteEntity(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 absPos);  // 渲染单个 2D 实体(absPos=绝对画布坐标)
    void RenderSpriteEntityAt(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 pos, glm::vec2 size); // 渲染精灵(锚点布局后精确位置+尺寸)
    void RenderTextEntity(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 absPos);    // 渲染单个文本实体
    void RenderButtonEntity(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 absPos);  // 渲染单个按钮实体(填充+文字)
    void RenderSlice9Entity(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 absPos);  // 渲染单个九宫格实体

    glm::mat4 m_WorldViewProj = glm::mat4(1.0f);
    glm::mat4 m_UIViewProj = glm::mat4(1.0f);
    uint32_t m_Width = 1920;
    uint32_t m_Height = 1080;
    glm::vec2 m_WorldCenter = { 0.0f, 0.0f };
    float m_WorldZoom = 1.0f;
    bool m_PrevMouseDown = false;   // 上一帧鼠标按下状态（点击边沿检测）
    bool m_RenderingUI = false;      // 仅 UI pass 使用全局透明度，世界层保持原材质
};

} // namespace UI
