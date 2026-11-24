// Canvas2D.cpp - 2D 场景图实现
#include "UI/Canvas2D.h"
#include "Core/Camera2DSystem.h"
#include "Rendering/TextRenderer.h"
#include "Core/TilemapSystem.h"
#include "Core/Physics2DSystem.h"
#include "Core/RenderGlobals.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace UI {

namespace {

glm::vec4 ApplyCanvasOpacity(glm::vec4 color, bool uiPass)
{
    if (uiPass) color.a *= GetUIOpacity();
    return color;
}

glm::vec2 MeasureTextBounds(const std::string& text, float fontSize, bool bitmap)
{
    TextRenderer& renderer = TextRenderer::GetInstance();
    const float width = std::max(0.0f, renderer.MeasureString(text, fontSize));
    float height = bitmap
        ? renderer.GetLineHeight(fontSize)
        : renderer.GetSdfLineHeight(fontSize);
    if (height <= 0.0f) height = std::max(fontSize, 0.0f);
    return glm::vec2(width, height);
}

struct Bounds {
    glm::vec2 position;
    glm::vec2 size;
};

template <class Sprite, class Text, class Button, class Slice9>
Bounds LayoutNode(const Bounds& parent, glm::vec2 offset, glm::vec2 scale,
                  const Sprite* sprite, const Text* text, const Button* button, const Slice9* slice)
{
    Bounds bounds{offset, parent.size};
    if (sprite && sprite->isUI) {
        Canvas2D::ComputeAnchorLayout(parent.size, sprite->anchorMin, sprite->anchorMax,
                                     offset, {sprite->width, sprite->height}, bounds.position, bounds.size);
        bounds.size *= scale;
    } else if (text && text->isUI) {
        const auto size = MeasureTextBounds(text->text, std::max(0.0f, text->fontSize * scale.x),
                                            text->renderMode == decltype(text->renderMode)::Bitmap);
        Canvas2D::ComputeAnchorLayout(parent.size, text->anchorMin, text->anchorMax,
                                     offset, size, bounds.position, bounds.size, text->pivot);
    } else if (button && button->isUI) {
        bounds.size = glm::vec2(button->width, button->height) * scale;
    } else if (slice && slice->isUI) {
        bounds.size = glm::vec2(slice->width, slice->height) * scale;
    }
    bounds.position += parent.position;
    return bounds;
}

template<class T>
T* Component(ECS::Coordinator& coordinator, ECS::Entity entity)
{
    return coordinator.HasComponent<T>(entity) ? &coordinator.GetComponent<T>(entity) : nullptr;
}

Bounds LiveBounds(ECS::Coordinator& coordinator, ECS::Entity entity, const Bounds& parent)
{
    const auto* transform = Component<ECS::TransformComponent>(coordinator, entity);
    return LayoutNode(parent, transform ? glm::vec2(transform->position) : glm::vec2(0.0f),
                      transform ? glm::vec2(transform->scale) : glm::vec2(1.0f),
                      Component<ECS::Sprite2DComponent>(coordinator, entity),
                      Component<ECS::TextComponent>(coordinator, entity),
                      Component<ECS::ButtonComponent>(coordinator, entity),
                      Component<ECS::Slice9Component>(coordinator, entity));
}

template<class Canvas>
Bounds CanvasBounds(const Canvas& canvas, glm::vec2 viewport)
{
    return {glm::vec2(0.0f), canvas.stretchToViewport ? viewport : glm::vec2(canvas.width, canvas.height)};
}

bool Contains(const Bounds& bounds, glm::vec2 point)
{
    const glm::vec2 begin = glm::min(bounds.position, bounds.position + bounds.size);
    const glm::vec2 end = glm::max(bounds.position, bounds.position + bounds.size);
    return point.x >= begin.x && point.x < end.x && point.y >= begin.y && point.y < end.y;
}

struct CanvasHit {
    ECS::Entity entity = ECS::INVALID_ENTITY;
    int layer = std::numeric_limits<int>::min();
    bool sprite = false;
};

void FindCanvasHit(ECS::Entity entity, const Bounds& parent, glm::vec2 mouse,
                   int depth, CanvasHit& hit)
{
    if (depth > 32) return;
    auto& scene = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.IsAlive(entity) || !scene.IsVisible(entity)) return;
    const Bounds bounds = LiveBounds(coordinator, entity, parent);
    const auto* sprite = Component<ECS::Sprite2DComponent>(coordinator, entity);
    const auto* text = Component<ECS::TextComponent>(coordinator, entity);
    const auto* button = Component<ECS::ButtonComponent>(coordinator, entity);
    if (sprite && sprite->isUI && sprite->type == ECS::Sprite2DComponent::Type::Button) {
        glm::vec2 logicalMouse = mouse;
        if (const auto* transform = Component<ECS::TransformComponent>(coordinator, entity)) {
            const auto center = bounds.position + bounds.size * 0.5f;
            const auto delta = mouse - center;
            const float angle = glm::radians(-transform->GetEulerAngles().z);
            const float cosine = std::cos(angle), sine = std::sin(angle);
            logicalMouse = center + glm::vec2(delta.x * cosine - delta.y * sine,
                                              delta.x * sine + delta.y * cosine);
        }
        if (sprite->layer >= hit.layer && Contains(bounds, logicalMouse))
            hit = {entity, sprite->layer, true};
    } else if ((!sprite || !sprite->isUI) && (!text || !text->isUI) && button && button->isUI) {
        if (button->layer >= hit.layer && Contains(bounds, mouse))
            hit = {entity, button->layer, false};
    }
    for (const auto child : scene.GetChildren(entity))
        FindCanvasHit(child, bounds, mouse, depth + 1, hit);
}

} // namespace

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
    m_PrevMouseDown = false;
    m_HoveredButton = {};
}

void Canvas2D::Update(glm::vec2 mouseScreenPos, bool mouseDown) {
    const bool clickEdge = mouseDown && !m_PrevMouseDown;
    m_PrevMouseDown = mouseDown;

    auto& scene = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    CanvasHit hit;
    if (Contains({glm::vec2(0.0f), GetViewportSize()}, mouseScreenPos)) {
        for (const auto entity : scene.GetRootEntities()) {
            const auto* canvas = Component<ECS::Canvas2DComponent>(coordinator, entity);
            if (!canvas || !scene.IsVisible(entity)) continue;
            const auto parent = CanvasBounds(*canvas, GetViewportSize());
            for (const auto child : scene.GetChildren(entity))
                FindCanvasHit(child, parent, mouseScreenPos, 0, hit);
        }
    }
    if (coordinator.IsAlive(m_HoveredButton)) {
        if (auto* sprite = Component<ECS::Sprite2DComponent>(coordinator, m_HoveredButton.entity)) sprite->hovered = false;
        if (auto* button = Component<ECS::ButtonComponent>(coordinator, m_HoveredButton.entity)) button->hovered = false;
    }
    m_HoveredButton = coordinator.GetEntityHandle(hit.entity);
    if (!m_HoveredButton.IsValid()) return;
    std::function<void()> callback;
    if (hit.sprite) {
        auto& sprite = coordinator.GetComponent<ECS::Sprite2DComponent>(hit.entity);
        sprite.hovered = true;
        if (clickEdge) callback = sprite.onClick;
    } else {
        auto& button = coordinator.GetComponent<ECS::ButtonComponent>(hit.entity);
        button.hovered = true;
        if (clickEdge) callback = button.onClick;
    }
    if (callback) callback();
}

bool Canvas2D::GetEntityLayout(ECS::Entity entity, glm::vec2& position, glm::vec2& size)
{
    return GetEntityLayout(entity, GetInstance().GetViewportSize(), position, size);
}

bool Canvas2D::GetEntityLayout(ECS::Entity entity, const glm::vec2& viewport,
                               glm::vec2& position, glm::vec2& size)
{
    auto& scene = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    std::vector<ECS::Entity> chain;
    while (coordinator.IsAlive(entity) && chain.size() <= 32) {
        if (const auto* canvas = Component<ECS::Canvas2DComponent>(coordinator, entity)) {
            if (chain.empty() || scene.GetParent(entity) != ECS::INVALID_ENTITY) return false;
            Bounds bounds = CanvasBounds(*canvas, viewport);
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                bounds = LiveBounds(coordinator, *it, bounds);
            position = bounds.position;
            size = bounds.size;
            return true;
        }
        chain.push_back(entity);
        entity = scene.GetParent(entity);
    }
    return false;
}

void Canvas2D::RenderWorld(Renderer2D& r2d, VkCommandBuffer cmd, const ::RenderWorld& world) {
    // 世界层（玩法：世界相机, 离屏管线, 与 3D 同受后处理）
    m_RenderingUI = false;
    r2d.BeginFrame(cmd, m_WorldViewProj, m_Width, m_Height, false);
    RenderECSNodes(r2d, world, false);
    r2d.Flush();
}

void Canvas2D::RenderUI(Renderer2D& r2d, VkCommandBuffer cmd, const ::RenderWorld& world) {
    // UI 层（屏幕坐标）：与玩法层同一离屏 render pass 内渲染（当前无 bloom 时无差别）。
    // 注：未来实现 bloom 时，需为 UI 建独立 LOAD_OP_LOAD render pass 在 bloom 之后叠加，UI 才不受影响。
    m_RenderingUI = true;
    r2d.BeginFrame(cmd, m_UIViewProj, m_Width, m_Height, false);
    RenderECSNodes(r2d, world, true);
    r2d.Flush();
    m_RenderingUI = false;
}

void Canvas2D::RenderECSNodes(Renderer2D& r2d, const ::RenderWorld& world, bool uiPass) {
    // Unity Canvas 模型：UI 实体作为 Canvas 父节点的子级（相对画布的屏幕坐标，支持嵌套层级累加）；
    // 世界层(isUI=false)实体作为根节点直接渲染（世界坐标）。
    for (const ECS::Entity entity : world.rootEntities) {
        const RenderWorldEntity* node = world.Find(entity);
        if (node == nullptr) continue;
        // 画布：UI 层渲染其子级（递归）
        if (node->hasCanvas) {
            if (uiPass) RenderCanvasChildren(r2d, world, entity);
            continue;
        }
        // 世界层 2D 实体：可见性已在 ECS -> RenderWorld 提取阶段求值。
        if (!node->visible) continue;
        const glm::vec2 absPos = node->hasTransform
            ? glm::vec2(node->transform.position.x, node->transform.position.y)
            : glm::vec2(0.0f);
        // 世界层实体（非 UI）：位置 = transform.position 世界坐标
        if (!uiPass && node->hasSprite && !node->sprite.isUI) {
            RenderSpriteEntity(r2d, *node, absPos);
        }
        // 世界层文本实体（isUI=false）
        if (!uiPass && node->hasText && !node->text.isUI) {
            RenderTextEntity(r2d, *node, absPos);
        }
        // 世界层按钮实体（isUI=false）
        if (!uiPass && node->hasButton && !node->button.isUI) {
            RenderButtonEntity(r2d, *node, absPos);
        }
        // 世界层九宫格实体（isUI=false）
        if (!uiPass && node->hasSlice9 && !node->slice9.isUI) {
            RenderSlice9Entity(r2d, *node, absPos);
        }
        // 瓦片地图(世界层; TMX 由 TilemapSystem 渲染, 与场景树 2D 实体同相机合批)
        if (!uiPass && node->hasTilemap) {
            TilemapSystem::GetInstance().Render(r2d, entity);
        }
    }

    // 2D 碰撞调试线框(T 键切换; 物理排错): 实体碰撞体(绿/青) + 瓦片碰撞体(红)
    if (!uiPass && g_ShowPhysics2DDebug) {
        Physics2DSystem::GetInstance().RenderDebug(r2d);
        TilemapSystem::GetInstance().RenderDebugColliders(r2d);
    }
}

void Canvas2D::RenderCanvasChildren(Renderer2D& r2d, const ::RenderWorld& world, ECS::Entity canvas) {
    const RenderWorldEntity* canvasNode = world.Find(canvas);
    if (!canvasNode || !canvasNode->visible) return;
    const Bounds layout = CanvasBounds(canvasNode->canvas, GetViewportSize());
    for (const ECS::Entity child : canvasNode->children) {
        RenderCanvasNodeRecursive(r2d, world, child, layout.position, layout.size, 0);
    }
}

void Canvas2D::RenderCanvasNodeRecursive(Renderer2D& r2d, const ::RenderWorld& world, ECS::Entity entity, glm::vec2 parentPos, glm::vec2 parentSize, int depth) {
    if (depth > 32) return;  // 防御: 历史数据可能存在环, 限制深度避免死循环
    // 可见性：实体或祖先被隐藏（SceneECS::SetVisible）→ 跳过自身与子树
    const RenderWorldEntity* node = world.Find(entity);
    if (node == nullptr || !node->visible) return;
    const glm::vec2 offset = node->hasTransform
        ? glm::vec2(node->transform.position.x, node->transform.position.y)
        : glm::vec2(0.0f);
    const auto bounds = LayoutNode({parentPos, parentSize}, offset,
        node->hasTransform ? glm::vec2(node->transform.scale) : glm::vec2(1.0f),
        node->hasSprite ? &node->sprite : nullptr,
        node->hasText ? &node->text : nullptr,
        node->hasButton ? &node->button : nullptr,
        node->hasSlice9 ? &node->slice9 : nullptr);
    if (node->hasSprite && node->sprite.isUI)
        RenderSpriteEntityAt(r2d, *node, bounds.position, bounds.size);
    else if (node->hasText && node->text.isUI)
        RenderTextEntity(r2d, *node, bounds.position);
    else {
        if (node->hasButton && node->button.isUI) RenderButtonEntity(r2d, *node, bounds.position);
        if (node->hasSlice9 && node->slice9.isUI) RenderSlice9Entity(r2d, *node, bounds.position);
    }
    for (const ECS::Entity child : node->children)
        RenderCanvasNodeRecursive(r2d, world, child, bounds.position, bounds.size, depth + 1);
}

void Canvas2D::RenderSpriteEntity(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 absPos) {
    if (!entity.hasSprite) return;
    const auto& s2d = entity.sprite;
    const glm::vec3 scale = entity.hasTransform ? entity.transform.scale : glm::vec3(1.0f);
    glm::vec2 size(s2d.width * scale.x, s2d.height * scale.y);
    RenderSpriteEntityAt(r2d, entity, absPos, size);
}

void Canvas2D::RenderSpriteEntityAt(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 pos, glm::vec2 size) {
    if (!entity.hasSprite) return;
    const auto& s2d = entity.sprite;
    VkDescriptorSet tex = s2d.texture.empty() ? r2d.GetWhiteTexture() : r2d.GetTexture(s2d.texture);

    // 2D 旋转(绕实体中心, z 轴)。旋转时手动构建四边形(四角旋转)。
    const float rotDeg = entity.hasTransform ? entity.transform.eulerAngles.z : 0.0f;
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
        q.color = ApplyCanvasOpacity(
            (s2d.type == RenderSpriteType::Button && s2d.hovered)
                ? glm::vec4(0.25f, 0.30f, 0.45f, 1.0f) : s2d.color,
            m_RenderingUI);
        q.texture = tex;
        q.layer = s2d.layer;
        r2d.DrawQuad(q);
        return;
    }

    switch (s2d.type) {
    case RenderSpriteType::Rect:
        r2d.DrawRect(pos, size, ApplyCanvasOpacity(s2d.color, m_RenderingUI), s2d.layer);
        break;
    case RenderSpriteType::Sprite:
        r2d.DrawSprite(pos, size, tex, s2d.uv0, s2d.uv1,
                       ApplyCanvasOpacity(s2d.color, m_RenderingUI), s2d.layer);
        break;
    case RenderSpriteType::Button: {
        glm::vec4 c = s2d.hovered ? glm::vec4(0.25f, 0.30f, 0.45f, 1.0f) : s2d.color;
        r2d.DrawRect(pos, size, ApplyCanvasOpacity(c, m_RenderingUI), s2d.layer);
        if (!s2d.texture.empty()) {
            glm::vec2 iconSize = size * 0.6f;
            r2d.DrawSprite(pos + (size - iconSize) * 0.5f, iconSize, tex,
                           s2d.uv0, s2d.uv1,
                           ApplyCanvasOpacity(glm::vec4(1.0f), m_RenderingUI),
                           s2d.layer);  // 同层(图标后画, 在按钮之上)
        }
        // 按钮标签（SDF 渲染，居中；字号 0 = 自动取按钮高度 30%）
        if (!s2d.label.empty()) {
            TextRenderer& tr = TextRenderer::GetInstance();
            float fs = s2d.labelFontSize > 0.0f ? s2d.labelFontSize : size.y * 0.3f;
            float tw = tr.MeasureString(s2d.label, fs);
            // 水平居中；baseline 使字形视觉中心贴近按钮中心（近似：中心 + 行高补偿）
            float tx = pos.x + (size.x - tw) * 0.5f;
            float ty = pos.y + size.y * 0.5f + fs * 0.10f;
            tr.DrawStringSdf(s2d.label, tx, ty, fs,
                             ApplyCanvasOpacity(s2d.labelColor, m_RenderingUI),
                             s2d.layer + 1);
        }
        break;
    }
    }
}

void Canvas2D::RenderTextEntity(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 absPos) {
    if (!entity.hasText) return;
    (void)r2d;
    const auto& tc = entity.text;
    // 字号可被 Transform.scale.x 整体缩放（与 Sprite 的 scale 语义一致）
    float fontSize = tc.fontSize;
    if (entity.hasTransform) {
        fontSize *= entity.transform.scale.x;
        if (fontSize <= 0.0f) return;
    }

    // 测量（供命中测试/包围盒，渲染时更新）
    TextRenderer& tr = TextRenderer::GetInstance();
    switch (tc.renderMode) {
    case RenderTextMode::Msdf:
        (void)tr.GetSdfLineHeight(fontSize);  // SDF/MSDF 行高一致（基础字号行高×缩放）
        break;
    case RenderTextMode::Sdf:
        (void)tr.GetSdfLineHeight(fontSize);
        break;
    default:
        (void)tr.GetLineHeight(fontSize);
        break;
    }

    // 位置 = absPos（左下角 / baseline 起点，与 TextRenderer 的 x/y 语义一致）
    switch (tc.renderMode) {
    case RenderTextMode::Msdf:
        tr.DrawStringMsdf(tc.text, absPos.x, absPos.y, fontSize,
                          ApplyCanvasOpacity(tc.color, m_RenderingUI), tc.layer);
        break;
    case RenderTextMode::Sdf:
        tr.DrawStringSdf(tc.text, absPos.x, absPos.y, fontSize,
                         ApplyCanvasOpacity(tc.color, m_RenderingUI), tc.layer);
        break;
    default:
        tr.DrawString(tc.text, absPos.x, absPos.y, fontSize,
                      ApplyCanvasOpacity(tc.color, m_RenderingUI), tc.layer);
        break;
    }
}

void Canvas2D::RenderButtonEntity(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 absPos) {
    if (!entity.hasButton) return;
    const auto& bc = entity.button;
    // 尺寸可被 Transform.scale 缩放（与 Sprite 一致）
    glm::vec2 size(bc.width, bc.height);
    if (entity.hasTransform) {
        size.x *= entity.transform.scale.x;
        size.y *= entity.transform.scale.y;
    }
    // 填充矩形（hover 变色）
    glm::vec4 fill = bc.hovered ? bc.hoverColor : bc.fillColor;
    r2d.DrawRect(absPos, size, ApplyCanvasOpacity(fill, m_RenderingUI), bc.layer);
    // 按钮文字（SDF 渲染，居中；字号 0 = 自动取按钮高度 30%）
    if (!bc.text.empty()) {
        TextRenderer& tr = TextRenderer::GetInstance();
        float fs = bc.fontSize > 0.0f ? bc.fontSize : size.y * 0.3f;
        float tw = tr.MeasureString(bc.text, fs);
        float tx = absPos.x + (size.x - tw) * 0.5f;
        float ty = absPos.y + size.y * 0.5f + fs * 0.10f;
        tr.DrawStringSdf(bc.text, tx, ty, fs,
                         ApplyCanvasOpacity(bc.textColor, m_RenderingUI), bc.layer + 1);
    }
}

void Canvas2D::RenderSlice9Entity(Renderer2D& r2d, const RenderWorldEntity& entity, glm::vec2 absPos) {
    if (!entity.hasSlice9) return;
    const auto& s9 = entity.slice9;
    // 尺寸可被 Transform.scale 缩放
    glm::vec2 size(s9.width, s9.height);
    if (entity.hasTransform) {
        size.x *= entity.transform.scale.x;
        size.y *= entity.transform.scale.y;
    }
    VkDescriptorSet tex = s9.texture.empty() ? r2d.GetWhiteTexture() : r2d.GetTexture(s9.texture);
    glm::vec2 srcSize(s9.texWidth, s9.texHeight);
    r2d.DrawSlice9(absPos, size, tex, s9.border, srcSize, s9.uv0, s9.uv1,
                   ApplyCanvasOpacity(s9.color, m_RenderingUI), s9.layer);
}

// ===== 锚点拉伸布局 =====
void Canvas2D::ComputeAnchorLayout(const glm::vec2& parentSize,
                                   const glm::vec2& anchorMin, const glm::vec2& anchorMax,
                                   const glm::vec2& posOffset, const glm::vec2& size,
                                   glm::vec2& outPos, glm::vec2& outSize,
                                   const glm::vec2& pivot) {
    outSize.x = parentSize.x * (anchorMax.x - anchorMin.x) + size.x;
    outSize.y = parentSize.y * (anchorMax.y - anchorMin.y) + size.y;
    // 保护：尺寸防止负值
    if (outSize.x < 0.0f) outSize.x = 0.0f;
    if (outSize.y < 0.0f) outSize.y = 0.0f;
    outPos.x = parentSize.x * anchorMin.x + posOffset.x - outSize.x * pivot.x;
    outPos.y = parentSize.y * anchorMin.y + posOffset.y - outSize.y * pivot.y;
}

} // namespace UI
