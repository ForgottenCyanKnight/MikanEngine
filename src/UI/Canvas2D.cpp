// Canvas2D.cpp - 2D 场景图实现
#include "UI/Canvas2D.h"
#include "Core/Camera2DSystem.h"
#include "Rendering/TextRenderer.h"
#include "Core/TilemapSystem.h"
#include "Core/Physics2DSystem.h"
#include "Core/RenderGlobals.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace UI {

// ===== Canvas2D =====
Canvas2D& Canvas2D::GetInstance() {
    static Canvas2D instance;
    return instance;
}

void Canvas2D::SetViewport(uint32_t width, uint32_t height) {
    m_Width = width;
    m_Height = height;
    // UI：画布坐标(左下原点, y 向下, 顶部=0)。glm::ortho 为 GL 约定(y 向上),
    // 但 Vulkan NDC y 相反且此处未 proj[1][1]*=-1 → 实际显示 y 向下,与画布约定一致。
    // 输入(屏幕像素 y 向下)与画布同向,无需翻转(下方 Update 中 node 树的翻转仅服务遗留死代码)。
    m_UIViewProj = glm::ortho(0.0f, (float)width, 0.0f, (float)height);
    // 世界：以 m_WorldCenter 为中心，zoom 缩放（同上, 显示 y 向下）
    float halfW = (float)width * 0.5f / m_WorldZoom;
    float halfH = (float)height * 0.5f / m_WorldZoom;
    m_WorldViewProj = glm::ortho(m_WorldCenter.x - halfW, m_WorldCenter.x + halfW,
                                 m_WorldCenter.y - halfH, m_WorldCenter.y + halfH);
}

void Canvas2D::SetWorldCamera(glm::vec2 center, float zoom) {
    m_WorldCenter = center;
    m_WorldZoom = std::max(zoom, 0.01f);
    SetViewport(m_Width, m_Height);  // 重建矩阵
}

void Canvas2D::SyncCameraFromScene() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    // Camera2DSystem 是 active camera 选择的唯一来源；这里不再复制一套场景树遍历规则。
    const ECS::Entity found = Camera2DSystem::GetInstance().GetActiveCamera();

    if (found != ECS::INVALID_ENTITY) {
        auto& c = coordinator.GetComponent<ECS::Camera2DComponent>(found);
        SetWorldCamera(c.center, c.zoom);
    } else {
        SetWorldCamera(glm::vec2(0.0f), 1.0f); // 无组件:默认(与旧行为一致)
    }
}

void Canvas2D::Clear() {
    // 独立 Node 子树已删除；ECS 场景树由 SceneECS 统一管理，此处无清理项
}

void Canvas2D::Update(glm::vec2 mouseScreenPos, bool mouseDown) {
    // 画布坐标 = 屏幕坐标(y 向下, 0=顶部), 与 ECS 按钮命中一致, 无需翻转。
    glm::vec2 mousePos = mouseScreenPos;
    // 点击边沿检测：仅"按下瞬间"触发 onClick（按住不重复触发）
    const bool clickEdge = mouseDown && !m_PrevMouseDown;
    m_PrevMouseDown = mouseDown;

    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto roots = sceneECS.GetRootEntities();
    for (auto entity : roots) {
        if (!coordinator.HasComponent<ECS::Canvas2DComponent>(entity)) continue;
        auto children = sceneECS.GetChildren(entity);
        for (auto child : children) {
            UpdateCanvasNodeRecursive(child, glm::vec2(0.0f), mousePos, mouseDown, clickEdge, 0);
        }
    }
}

void Canvas2D::UpdateCanvasNodeRecursive(ECS::Entity entity, glm::vec2 parentPos, glm::vec2 mousePos, bool mouseDown, bool clickEdge, int depth) {
    if (depth > 32) return;  // 防御: 防环死循环
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    glm::vec2 local(0.0f);
    if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
        auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
        local = glm::vec2(t.position.x, t.position.y);
    }
    const glm::vec2 worldPos = parentPos + local;
    // 自身按钮命中
    if (coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
        auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
        if (s2d.isUI && s2d.type == ECS::Sprite2DComponent::Type::Button) {
            glm::vec2 size(s2d.width, s2d.height);
            bool hit = mousePos.x >= worldPos.x && mousePos.x <= worldPos.x + size.x &&
                       mousePos.y >= worldPos.y && mousePos.y <= worldPos.y + size.y;
            s2d.hovered = hit;
            if (hit && clickEdge) {
                if (s2d.onClick) s2d.onClick();
                else printf("[UI] Button clicked (entity=%u, label='%s')\n", (uint32_t)entity, s2d.label.c_str());
            }
        }
    }
    // 独立 ButtonComponent 命中（新 ECS 按钮）
    if (coordinator.HasComponent<ECS::ButtonComponent>(entity)) {
        auto& bc = coordinator.GetComponent<ECS::ButtonComponent>(entity);
        if (bc.isUI) {
            glm::vec2 size(bc.width, bc.height);
            bool hit = mousePos.x >= worldPos.x && mousePos.x <= worldPos.x + size.x &&
                       mousePos.y >= worldPos.y && mousePos.y <= worldPos.y + size.y;
            bc.hovered = hit;
            if (hit && clickEdge) {
                if (bc.onClick) bc.onClick();
                else printf("[UI] ButtonComponent clicked (entity=%u, text='%s')\n", (uint32_t)entity, bc.text.c_str());
            }
        }
    }
    // 递归子级
    auto children = sceneECS.GetChildren(entity);
    for (auto child : children) {
        UpdateCanvasNodeRecursive(child, worldPos, mousePos, mouseDown, clickEdge, depth + 1);
    }
}

void Canvas2D::RenderWorld(Renderer2D& r2d, VkCommandBuffer cmd) {
    // 世界层（玩法：世界相机, 离屏管线, 与 3D 同受后处理）
    r2d.BeginFrame(cmd, m_WorldViewProj, m_Width, m_Height, false);
    RenderECSNodes(r2d, false);
    r2d.Flush();
}

void Canvas2D::RenderUI(Renderer2D& r2d, VkCommandBuffer cmd) {
    // UI 层（屏幕坐标）：与玩法层同一离屏 render pass 内渲染（当前无 bloom 时无差别）。
    // 注：未来实现 bloom 时，需为 UI 建独立 LOAD_OP_LOAD render pass 在 bloom 之后叠加，UI 才不受影响。
    r2d.BeginFrame(cmd, m_UIViewProj, m_Width, m_Height, false);
    RenderECSNodes(r2d, true);
    r2d.Flush();
}

void Canvas2D::RenderECSNodes(Renderer2D& r2d, bool uiPass) {
    // Unity Canvas 模型：UI 实体作为 Canvas 父节点的子级（相对画布的屏幕坐标，支持嵌套层级累加）；
    // 世界层(isUI=false)实体作为根节点直接渲染（世界坐标）。
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto roots = sceneECS.GetRootEntities();
    for (auto entity : roots) {
        // 画布：UI 层渲染其子级（递归）
        if (coordinator.HasComponent<ECS::Canvas2DComponent>(entity)) {
            if (uiPass) RenderCanvasChildren(r2d, entity);
            continue;
        }
        // 世界层 2D 实体：可见性检查（SceneECS::IsVisible，支持 Sprite2D/Text/Button/Slice9）
        if (!sceneECS.IsVisible(entity)) continue;
        // 世界层实体（非 UI）：位置 = transform.position 世界坐标
        if (!uiPass && coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
            glm::vec2 absPos(0.0f);
            if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {                auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
                absPos = glm::vec2(t.position.x, t.position.y);
            }
            RenderSpriteEntity(r2d, entity, absPos);
        }
        // 世界层文本实体（isUI=false）
        if (!uiPass && coordinator.HasComponent<ECS::TextComponent>(entity)) {
            auto& tc = coordinator.GetComponent<ECS::TextComponent>(entity);
            if (!tc.isUI) {
                glm::vec2 absPos(0.0f);
                if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
                    auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
                    absPos = glm::vec2(t.position.x, t.position.y);
                }
                RenderTextEntity(r2d, entity, absPos);
            }
        }
        // 世界层按钮实体（isUI=false）
        if (!uiPass && coordinator.HasComponent<ECS::ButtonComponent>(entity)) {
            auto& bc = coordinator.GetComponent<ECS::ButtonComponent>(entity);
            if (!bc.isUI) {
                glm::vec2 absPos(0.0f);
                if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
                    auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
                    absPos = glm::vec2(t.position.x, t.position.y);
                }
                RenderButtonEntity(r2d, entity, absPos);
            }
        }
        // 世界层九宫格实体（isUI=false）
        if (!uiPass && coordinator.HasComponent<ECS::Slice9Component>(entity)) {
            auto& s9 = coordinator.GetComponent<ECS::Slice9Component>(entity);
            if (!s9.isUI) {
                glm::vec2 absPos(0.0f);
                if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
                    auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
                    absPos = glm::vec2(t.position.x, t.position.y);
                }
                RenderSlice9Entity(r2d, entity, absPos);
            }
        }
        // 瓦片地图(世界层; TMX 由 TilemapSystem 渲染, 与场景树 2D 实体同相机合批)
        if (!uiPass && coordinator.HasComponent<ECS::TilemapComponent>(entity)) {
            TilemapSystem::GetInstance().Render(r2d, entity);
        }
    }

    // 2D 碰撞调试线框(T 键切换; 物理排错): 实体碰撞体(绿/青) + 瓦片碰撞体(红)
    if (!uiPass && g_ShowPhysics2DDebug) {
        Physics2DSystem::GetInstance().RenderDebug(r2d);
        TilemapSystem::GetInstance().RenderDebugColliders(r2d);
    }
}

void Canvas2D::RenderCanvasChildren(Renderer2D& r2d, ECS::Entity canvas) {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    // 取画布尺寸作为根锚点父容器
    glm::vec2 canvasSize(1920.0f, 1080.0f);
    if (coordinator.HasComponent<ECS::Canvas2DComponent>(canvas)) {
        auto& cc = coordinator.GetComponent<ECS::Canvas2DComponent>(canvas);
        canvasSize = glm::vec2(cc.width, cc.height);
    }
    // UI 原点固定屏幕(0,0)：与 Canvas 的 3D transform 无关（Canvas 仅作层级容器）
    auto children = sceneECS.GetChildren(canvas);
    for (auto child : children) {
        RenderCanvasNodeRecursive(r2d, child, glm::vec2(0.0f), canvasSize, 0);
    }
}

void Canvas2D::RenderCanvasNodeRecursive(Renderer2D& r2d, ECS::Entity entity, glm::vec2 parentPos, glm::vec2 parentSize, int depth) {
    if (depth > 32) return;  // 防御: 历史数据可能存在环, 限制深度避免死循环
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    // 可见性：实体或祖先被隐藏（SceneECS::SetVisible）→ 跳过自身与子树
    if (!sceneECS.IsVisible(entity)) return;
    // 自身局部位置
    glm::vec2 local(0.0f);
    if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
        auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
        local = glm::vec2(t.position.x, t.position.y);
    }
    const glm::vec2 worldPos = parentPos + local;  // 父链累加的绝对画布坐标（无锚点时的回退）
    // 子级视作父容器尺寸：自身 size（Sprite2D 取其 width/height；否则用父尺寸传递）
    glm::vec2 selfSize = parentSize; // 默认透传父尺寸

    bool handled = false; // 是否已通过锚点布局渲染
    // 渲染自身 — Sprite2D（UI 层支持锚点拉伸布局）
    if (coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
        auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
        if (s2d.isUI) {
            glm::vec2 anchoredPos, anchoredSize;
            ComputeAnchorLayout(parentSize, s2d.anchorMin, s2d.anchorMax, local,
                                glm::vec2(s2d.width, s2d.height), anchoredPos, anchoredSize);
            // Transform.scale 叠加锚点后尺寸
            if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
                auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
                anchoredSize.x *= t.scale.x;
                anchoredSize.y *= t.scale.y;
            }
            selfSize = anchoredSize;
            // 用锚点位置+尺寸渲染精灵
            RenderSpriteEntityAt(r2d, entity, anchoredPos, anchoredSize);
            handled = true;
        }
    }
    if (!handled) {
        // 非锚点路径：文本、按钮、九宫格、世界层精灵
        if (coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
            auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
            if (s2d.isUI) RenderSpriteEntity(r2d, entity, worldPos);
        }
        if (coordinator.HasComponent<ECS::TextComponent>(entity)) {
            auto& tc = coordinator.GetComponent<ECS::TextComponent>(entity);
            if (tc.isUI) RenderTextEntity(r2d, entity, worldPos);
        }
        if (coordinator.HasComponent<ECS::ButtonComponent>(entity)) {
            auto& bc = coordinator.GetComponent<ECS::ButtonComponent>(entity);
            if (bc.isUI) RenderButtonEntity(r2d, entity, worldPos);
        }
        if (coordinator.HasComponent<ECS::Slice9Component>(entity)) {
            auto& s9 = coordinator.GetComponent<ECS::Slice9Component>(entity);
            if (s9.isUI) RenderSlice9Entity(r2d, entity, worldPos);
        }
    }
    // 递归子级
    auto children = sceneECS.GetChildren(entity);
    for (auto child : children) {
        RenderCanvasNodeRecursive(r2d, child, worldPos, selfSize, depth + 1);
    }
}

void Canvas2D::RenderSpriteEntity(Renderer2D& r2d, ECS::Entity entity, glm::vec2 absPos) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
    auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
    glm::vec2 size(s2d.width * t.scale.x, s2d.height * t.scale.y);
    RenderSpriteEntityAt(r2d, entity, absPos, size);
}

void Canvas2D::RenderSpriteEntityAt(Renderer2D& r2d, ECS::Entity entity, glm::vec2 pos, glm::vec2 size) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
    auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
    VkDescriptorSet tex = s2d.texture.empty() ? r2d.GetWhiteTexture() : r2d.GetTexture(s2d.texture);

    // 2D 旋转(绕实体中心, z 轴)。旋转时手动构建四边形(四角旋转)。
    const float rotDeg = t.GetEulerAngles().z;
    if (fabsf(rotDeg) > 0.01f) {
        const float rad = glm::radians(rotDeg);
        const float cA = cosf(rad), sA = sinf(rad);
        const glm::vec2 c = pos + size * 0.5f;
        const glm::vec2 half = size * 0.5f;
        Quad2D q;
        glm::vec2 corners[4] = {
            glm::vec2(-half.x, -half.y), glm::vec2(half.x, -half.y),
            glm::vec2(half.x,  half.y),  glm::vec2(-half.x,  half.y)
        };
        for (int i = 0; i < 4; i++) {
            glm::vec2 r(corners[i].x * cA - corners[i].y * sA,
                        corners[i].x * sA + corners[i].y * cA);
            glm::vec2 v = c + r;
            if (i == 0) q.p0 = v; else if (i == 1) q.p1 = v;
            else if (i == 2) q.p2 = v; else q.p3 = v;
        }
        q.uv0 = s2d.uv0; q.uv1 = s2d.uv1;
        q.color = (s2d.type == ECS::Sprite2DComponent::Type::Button && s2d.hovered)
                  ? glm::vec4(0.25f, 0.30f, 0.45f, 1.0f) : s2d.color;
        q.texture = tex;
        q.layer = s2d.layer;
        r2d.DrawQuad(q);
        return;
    }

    switch (s2d.type) {
    case ECS::Sprite2DComponent::Type::Rect:
        r2d.DrawRect(pos, size, s2d.color, s2d.layer);
        break;
    case ECS::Sprite2DComponent::Type::Sprite:
        r2d.DrawSprite(pos, size, tex, s2d.uv0, s2d.uv1, s2d.color, s2d.layer);
        break;
    case ECS::Sprite2DComponent::Type::Button: {
        glm::vec4 c = s2d.hovered ? glm::vec4(0.25f, 0.30f, 0.45f, 1.0f) : s2d.color;
        r2d.DrawRect(pos, size, c, s2d.layer);
        if (!s2d.texture.empty()) {
            glm::vec2 iconSize = size * 0.6f;
            r2d.DrawSprite(pos + (size - iconSize) * 0.5f, iconSize, tex,
                           s2d.uv0, s2d.uv1, glm::vec4(1.0f), s2d.layer);  // 同层(图标后画, 在按钮之上)
        }
        // 按钮标签（SDF 渲染，居中；字号 0 = 自动取按钮高度 30%）
        if (!s2d.label.empty()) {
            TextRenderer& tr = TextRenderer::GetInstance();
            float fs = s2d.labelFontSize > 0.0f ? s2d.labelFontSize : size.y * 0.3f;
            float tw = tr.MeasureString(s2d.label, fs);
            // 水平居中；baseline 使字形视觉中心贴近按钮中心（近似：中心 + 行高补偿）
            float tx = pos.x + (size.x - tw) * 0.5f;
            float ty = pos.y + size.y * 0.5f + fs * 0.10f;
            tr.DrawStringSdf(s2d.label, tx, ty, fs, s2d.labelColor, s2d.layer + 1);
        }
        break;
    }
    }
}

void Canvas2D::RenderTextEntity(Renderer2D& r2d, ECS::Entity entity, glm::vec2 absPos) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& tc = coordinator.GetComponent<ECS::TextComponent>(entity);
    // 字号可被 Transform.scale.x 整体缩放（与 Sprite 的 scale 语义一致）
    float fontSize = tc.fontSize;
    if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
        auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
        fontSize *= t.scale.x;
        if (fontSize <= 0.0f) return;
    }

    // 测量（供命中测试/包围盒，渲染时更新）
    TextRenderer& tr = TextRenderer::GetInstance();
    tc.measuredWidth = tr.MeasureString(tc.text, fontSize);
    switch (tc.renderMode) {
    case ECS::TextComponent::RenderMode::Msdf:
        tc.measuredHeight = tr.GetSdfLineHeight(fontSize);  // SDF/MSDF 行高一致（基础字号行高×缩放）
        break;
    case ECS::TextComponent::RenderMode::Sdf:
        tc.measuredHeight = tr.GetSdfLineHeight(fontSize);
        break;
    default:
        tc.measuredHeight = tr.GetLineHeight(fontSize);
        break;
    }

    // 位置 = absPos（左下角 / baseline 起点，与 TextRenderer 的 x/y 语义一致）
    switch (tc.renderMode) {
    case ECS::TextComponent::RenderMode::Msdf:
        tr.DrawStringMsdf(tc.text, absPos.x, absPos.y, fontSize, tc.color, tc.layer);
        break;
    case ECS::TextComponent::RenderMode::Sdf:
        tr.DrawStringSdf(tc.text, absPos.x, absPos.y, fontSize, tc.color, tc.layer);
        break;
    default:
        tr.DrawString(tc.text, absPos.x, absPos.y, fontSize, tc.color, tc.layer);
        break;
    }
}

void Canvas2D::RenderButtonEntity(Renderer2D& r2d, ECS::Entity entity, glm::vec2 absPos) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& bc = coordinator.GetComponent<ECS::ButtonComponent>(entity);
    // 尺寸可被 Transform.scale 缩放（与 Sprite 一致）
    glm::vec2 size(bc.width, bc.height);
    if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
        auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
        size.x *= t.scale.x;
        size.y *= t.scale.y;
    }
    // 填充矩形（hover 变色）
    glm::vec4 fill = bc.hovered ? bc.hoverColor : bc.fillColor;
    r2d.DrawRect(absPos, size, fill, bc.layer);
    // 按钮文字（SDF 渲染，居中；字号 0 = 自动取按钮高度 30%）
    if (!bc.text.empty()) {
        TextRenderer& tr = TextRenderer::GetInstance();
        float fs = bc.fontSize > 0.0f ? bc.fontSize : size.y * 0.3f;
        float tw = tr.MeasureString(bc.text, fs);
        float tx = absPos.x + (size.x - tw) * 0.5f;
        float ty = absPos.y + size.y * 0.5f + fs * 0.10f;
        tr.DrawStringSdf(bc.text, tx, ty, fs, bc.textColor, bc.layer + 1);
    }
}

void Canvas2D::RenderSlice9Entity(Renderer2D& r2d, ECS::Entity entity, glm::vec2 absPos) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& s9 = coordinator.GetComponent<ECS::Slice9Component>(entity);
    // 尺寸可被 Transform.scale 缩放
    glm::vec2 size(s9.width, s9.height);
    if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
        auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
        size.x *= t.scale.x;
        size.y *= t.scale.y;
    }
    VkDescriptorSet tex = s9.texture.empty() ? r2d.GetWhiteTexture() : r2d.GetTexture(s9.texture);
    glm::vec2 srcSize(s9.texWidth, s9.texHeight);
    r2d.DrawSlice9(absPos, size, tex, s9.border, srcSize, s9.uv0, s9.uv1, s9.color, s9.layer);
}

// ===== 锚点拉伸布局 =====
void Canvas2D::ComputeAnchorLayout(const glm::vec2& parentSize,
                                   const glm::vec2& anchorMin, const glm::vec2& anchorMax,
                                   const glm::vec2& posOffset, const glm::vec2& size,
                                   glm::vec2& outPos, glm::vec2& outSize) {
    outPos.x = parentSize.x * anchorMin.x + posOffset.x;
    outPos.y = parentSize.y * anchorMin.y + posOffset.y;
    outSize.x = parentSize.x * (anchorMax.x - anchorMin.x) + size.x;
    outSize.y = parentSize.y * (anchorMax.y - anchorMin.y) + size.y;
    // 保护：尺寸防止负值
    if (outSize.x < 0.0f) outSize.x = 0.0f;
    if (outSize.y < 0.0f) outSize.y = 0.0f;
}

} // namespace UI
