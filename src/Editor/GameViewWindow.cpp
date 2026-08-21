#include "Editor/GameViewWindow.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "UI/Canvas2D.h"
#include "Editor/ToolbarWindow.h"
#include "Editor/GizmoMode.h"
#include <glm/glm.hpp>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>

namespace Editor {

GameViewWindow& GameViewWindow::GetInstance() {
    static GameViewWindow instance;
    return instance;
}

void GameViewWindow::Render(bool& showWindow) {
    if (!showWindow) {
        m_isVisible = false;
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("游戏视图", &showWindow);

    bool isCollapsed = ImGui::IsWindowCollapsed();
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    float titleBarHeight = ImGui::GetFrameHeight();
    float windowHeight = ImGui::GetWindowHeight();
    bool hasVisibleContent = (windowHeight > titleBarHeight + 10.0f) && (contentSize.x > 1.0f && contentSize.y > 1.0f);

    ImGuiWindow* window = ImGui::FindWindowByName("游戏视图");
    bool isActiveTab = window ? (window->Flags & ImGuiWindowFlags_DockNodeHost) == 0 : true;
    if (window && window->DockNode) {
        isActiveTab = (window->DockNode->VisibleWindow == window);
    }

    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    bool isOnScreen = (windowPos.x < viewport->WorkPos.x + viewport->WorkSize.x &&
                       windowPos.x + windowSize.x > viewport->WorkPos.x &&
                       windowPos.y < viewport->WorkPos.y + viewport->WorkSize.y &&
                       windowPos.y + windowSize.y > viewport->WorkPos.y);

    m_isVisible = !isCollapsed && hasVisibleContent && isOnScreen && isActiveTab;

    if (m_descriptorSet != VK_NULL_HANDLE) {
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);

        // 用 AddImage 直接绘制离屏纹理（不产生 ImGui item，避免干扰 2D gizmo 的鼠标交互）
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
        ImVec2 p0(windowPos.x + contentMin.x, windowPos.y + contentMin.y);
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)m_descriptorSet, p0,
            ImVec2(p0.x + contentSize.x, p0.y + contentSize.y), uv0, uv1);

        // ===== 2D UI 实体 gizmo（自绘：轴箭头/圆环扇形/缩放手柄）=====
        DrawUIEntityGizmo(contentSize);
    } else {
        ImVec2 contentAvail = ImGui::GetContentRegionAvail();
        ImVec2 textPos = ImVec2(
            ImGui::GetWindowPos().x + contentAvail.x * 0.5f - 50,
            ImGui::GetWindowPos().y + contentAvail.y * 0.5f
        );
        ImGui::SetCursorScreenPos(textPos);
        ImGui::Text("游戏视图");

        ImGui::SetCursorScreenPos(ImVec2(
            ImGui::GetWindowPos().x + contentAvail.x * 0.5f - 80,
            textPos.y + 20
        ));
        ImGui::TextDisabled("(游戏运行画面将显示在这里)");
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

// ==================== 自绘 2D gizmo ====================
// 不依赖 ImGuizmo(它是 3D 库, 2D 正交模式单轴位移失效且无 2D 扇形/手柄语义)。
// 2D 变换手柄按业界标准(Aseprite/Tiled)自绘: 轴箭头/圆环+扇形/缩放手柄 + 数值文字。
void GameViewWindow::DrawUIEntityGizmo(const ImVec2& contentSize) {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    ECS::Entity selected = sceneECS.GetSelectedEntity();
    if (selected == ECS::INVALID_ENTITY) return;
    if (!coordinator.HasComponent<ECS::TransformComponent>(selected)) return;

    // 支持 Sprite/Button/Text 三种 UI 组件（任一即可），获取 isUI 标志与逻辑尺寸
    bool isUI = false;
    glm::vec2 uiSize(0.0f);
    if (coordinator.HasComponent<ECS::Sprite2DComponent>(selected)) {
        auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(selected);
        isUI = s2d.isUI;
        uiSize = glm::vec2(s2d.width, s2d.height);
    } else if (coordinator.HasComponent<ECS::ButtonComponent>(selected)) {
        auto& bc = coordinator.GetComponent<ECS::ButtonComponent>(selected);
        isUI = bc.isUI;
        uiSize = glm::vec2(bc.width, bc.height);
    } else if (coordinator.HasComponent<ECS::TextComponent>(selected)) {
        auto& tc = coordinator.GetComponent<ECS::TextComponent>(selected);
        isUI = tc.isUI;
        // Text 用渲染时测量的包围盒（已含 Transform.scale 缩放）
        uiSize = glm::vec2(tc.measuredWidth, tc.measuredHeight);
    }
    if (!isUI) return;
    ECS::Entity parent = sceneECS.GetParent(selected);
    if (parent == ECS::INVALID_ENTITY) return;
    if (!coordinator.HasComponent<ECS::Canvas2DComponent>(parent)) return;

    if (!ToolbarWindow::GetInstance().IsShowGizmoAxis()) return;

    auto& canvas = coordinator.GetComponent<ECS::Canvas2DComponent>(parent);
    float cw = canvas.width, ch = canvas.height;
    auto& t = coordinator.GetComponent<ECS::TransformComponent>(selected);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    GizmoMode mode = ToolbarWindow::GetInstance().GetGizmoMode();
    const bool down = ImGui::GetIO().MouseDown[0];
    const ImVec2 mousePos = ImGui::GetIO().MousePos;

    // 屏幕映射（画布坐标 → GameView 屏幕）
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
    ImVec2 origin(windowPos.x + contentMin.x, windowPos.y + contentMin.y);
    auto screenFromCanvas = [&](glm::vec2 cp) -> ImVec2 {
        return ImVec2(origin.x + cp.x / cw * contentSize.x,
                      origin.y + cp.y / ch * contentSize.y);
    };
    // gizmo 中心 = 2D 组件几何中心(左上角 + 尺寸一半), 而非左上角原点
    // Text 的 measured 已含 scale；Sprite/Button 尺寸需乘 Transform.scale
    // 锚点实体：中心必须用锚点布局后的渲染位置（与 Canvas2D::ComputeAnchorLayout 一致），
    // 否则 position 只是相对锚点的偏移，gizmo 会显示在错误位置
    glm::vec2 objSize = uiSize;
    glm::vec2 basePos = glm::vec2(t.position.x, t.position.y);
    if (coordinator.HasComponent<ECS::Sprite2DComponent>(selected)) {
        auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(selected);
        glm::vec2 aPos, aSize;
        UI::Canvas2D::ComputeAnchorLayout(glm::vec2(cw, ch), s2d.anchorMin, s2d.anchorMax,
                                          basePos, uiSize, aPos, aSize);
        basePos = aPos;
        objSize = aSize;
        objSize.x *= t.scale.x;
        objSize.y *= t.scale.y;
    } else if (!coordinator.HasComponent<ECS::TextComponent>(selected)) {
        objSize.x *= t.scale.x;
        objSize.y *= t.scale.y;
    }
    glm::vec2 objCenter = basePos + objSize * 0.5f;
    ImVec2 center = screenFromCanvas(objCenter);
    const float sx = (contentSize.x > 0.0f) ? cw / contentSize.x : 1.0f;
    const float sy = (contentSize.y > 0.0f) ? ch / contentSize.y : 1.0f;
    const float R = 45.0f;   // gizmo 半径(屏幕 px)

    const ImU32 COL_X   = IM_COL32(220, 90, 90, 255);
    const ImU32 COL_Y   = IM_COL32(90, 200, 110, 255);
    const ImU32 COL_BLUE = IM_COL32(70, 130, 255, 255);
    const ImU32 COL_GRAY = IM_COL32(210, 210, 210, 255);
    const ImU32 COL_HL   = IM_COL32(255, 210, 60, 255);
    const ImU32 COL_FAN  = IM_COL32(255, 210, 60, 90);

    // ===== 命中检测与拖动状态 =====
    enum { H_NONE = -1, H_CENTER, H_X, H_Y, H_ROT, H_SX, H_SY };
    static int  s_handle = H_NONE;
    static bool s_drag = false;
    static int  s_rel = 0;
    static ImVec2    s_lastMouse  = ImVec2(0.0f, 0.0f);
    static glm::vec2 s_startPos   = glm::vec2(0.0f);
    static ImVec2    s_startScreen = ImVec2(0.0f, 0.0f);
    static float     s_startAngle = 0.0f;
    static float     s_startRotZ  = 0.0f;
    static glm::vec2 s_startScale = glm::vec2(1.0f);

    // 点到线段距离(屏幕 px) —— 轴精确命中
    auto pointToSegment = [](const ImVec2& p, const ImVec2& a, const ImVec2& b) -> float {
        ImVec2 ab(b.x - a.x, b.y - a.y), ap(p.x - a.x, p.y - a.y);
        float len2 = ab.x * ab.x + ab.y * ab.y;
        float t = (len2 > 1e-6f) ? (ap.x * ab.x + ap.y * ab.y) / len2 : 0.0f;
        t = glm::clamp(t, 0.0f, 1.0f);
        ImVec2 proj(a.x + ab.x * t, a.y + ab.y * t);
        float dx = p.x - proj.x, dy = p.y - proj.y;
        return sqrtf(dx * dx + dy * dy);
    };

    auto hitTest = [&](const ImVec2& mp) -> int {
        float dx = mp.x - center.x, dy = mp.y - center.y;
        float d = sqrtf(dx * dx + dy * dy);
        if (mode == GizmoMode::Rotate)
            return (fabsf(d - R) < 14.0f) ? H_ROT : H_NONE;
        if (mode == GizmoMode::Scale) {
            if (fabsf(mp.x - center.x) < 10.0f && fabsf(mp.y - center.y) < 10.0f) return H_CENTER;
            if (fabsf(mp.x - (center.x + R)) < 10.0f && fabsf(mp.y - center.y) < 10.0f) return H_SX;
            if (fabsf(mp.y - (center.y + R)) < 10.0f && fabsf(mp.x - center.x) < 10.0f) return H_SY;
            return H_NONE;
        }
        // Translate: 精确命中轴(点到线段 < 8px)或中心方块; 空白区不命中(避免误触)
        if (fabsf(dx) < 8.0f && fabsf(dy) < 8.0f) return H_CENTER;
        float dX = pointToSegment(mp, center, ImVec2(center.x + R + 10, center.y));
        float dY = pointToSegment(mp, center, ImVec2(center.x, center.y + R + 10));
        if (dX < 8.0f && dY < 8.0f) return (dX <= dY) ? H_X : H_Y;
        if (dX < 8.0f) return H_X;
        if (dY < 8.0f) return H_Y;
        return H_NONE;
    };

    // 悬停命中(每帧, 未拖动时): 轴/手柄悬停时黄色高亮, 只有命中才能开始拖动
    int hoverHandle = H_NONE;
    if (!s_drag) hoverHandle = hitTest(mousePos);

    if (!s_drag && down && hoverHandle != H_NONE) {
        s_drag = true;
        s_handle = hoverHandle;  // 只有悬停命中(gizmo 上)才能开始拖动
        s_rel = 0;
        s_lastMouse = mousePos; s_startPos = t.position;
        s_startScreen = screenFromCanvas(s_startPos);
        if (mode == GizmoMode::Rotate) {
            s_startAngle = atan2f(mousePos.y - center.y, mousePos.x - center.x);
            s_startRotZ = t.GetEulerAngles().z;
        } else if (mode == GizmoMode::Scale) {
            s_startScale = glm::vec2(t.scale.x, t.scale.y);
        }
    }
    if (s_drag) {
        if (down) {
            s_rel = 0;
            ImVec2 delta(mousePos.x - s_lastMouse.x, mousePos.y - s_lastMouse.y);
            s_lastMouse = mousePos;
            if (mode == GizmoMode::Translate) {
                if (s_handle == H_X) t.position.x += delta.x * sx;
                else if (s_handle == H_Y) t.position.y += delta.y * sy;
                else { t.position.x += delta.x * sx; t.position.y += delta.y * sy; }
            } else if (mode == GizmoMode::Rotate) {
                float a = atan2f(mousePos.y - center.y, mousePos.x - center.x);
                float da = a - s_startAngle;
                glm::vec3 e = t.GetEulerAngles();
                e.z = s_startRotZ + glm::degrees(da);
                t.SetEulerAngles(e);
            } else if (mode == GizmoMode::Scale) {
                // 增量累加(不是覆盖): 每帧鼠标位移 → scale
                if (s_handle == H_SX) t.scale.x += delta.x * sx * 0.005f;
                else if (s_handle == H_SY) t.scale.y += delta.y * sy * 0.005f;
                else {  // 中心: 两轴同时缩放
                    t.scale.x += delta.x * sx * 0.005f;
                    t.scale.y += delta.y * sy * 0.005f;
                }
                t.scale.x = glm::max(0.01f, t.scale.x);
                t.scale.y = glm::max(0.01f, t.scale.y);
            }
        } else if (++s_rel > 3) { s_drag = false; s_handle = H_NONE; }
    }

    // ===== 绘制 =====
    if (mode == GizmoMode::Translate) {
        bool hlX = (s_drag && s_handle == H_X) || (!s_drag && hoverHandle == H_X);
        bool hlY = (s_drag && s_handle == H_Y) || (!s_drag && hoverHandle == H_Y);
        bool hlC = (s_drag && s_handle == H_CENTER) || (!s_drag && hoverHandle == H_CENTER);
        // X 轴(向右): 线延伸到箭头底部, 箭头紧贴线端点(无缝隙)
        dl->AddLine(center, ImVec2(center.x + R + 10, center.y), hlX ? COL_HL : COL_X, 3.0f);
        dl->AddTriangleFilled(ImVec2(center.x + R + 10, center.y - 6), ImVec2(center.x + R + 10, center.y + 6), ImVec2(center.x + R + 20, center.y), hlX ? COL_HL : COL_X);
        // Y 轴(向下, 屏幕 y 向下): 同上
        dl->AddLine(center, ImVec2(center.x, center.y + R + 10), hlY ? COL_HL : COL_Y, 3.0f);
        dl->AddTriangleFilled(ImVec2(center.x - 6, center.y + R + 10), ImVec2(center.x + 6, center.y + R + 10), ImVec2(center.x, center.y + R + 20), hlY ? COL_HL : COL_Y);
        dl->AddRectFilled(ImVec2(center.x - 6, center.y - 6), ImVec2(center.x + 6, center.y + 6), hlC ? COL_HL : COL_BLUE);
        // 拖拽: 灰白位移线 + 数值
        if (s_drag) {
            ImVec2 cur = screenFromCanvas(glm::vec2(t.position.x, t.position.y));
            dl->AddCircle(s_startScreen, 6.0f, COL_GRAY);
            dl->AddCircle(cur, 6.0f, COL_GRAY);
            ImVec2 dif(cur.x - s_startScreen.x, cur.y - s_startScreen.y);
            float dLen = sqrtf(dif.x * dif.x + dif.y * dif.y);
            ImVec2 dir = (dLen > 0.01f) ? ImVec2(dif.x / dLen * 5.0f, dif.y / dLen * 5.0f) : ImVec2(0.0f, 0.0f);
            dl->AddLine(ImVec2(s_startScreen.x + dir.x, s_startScreen.y + dir.y),
                        ImVec2(cur.x - dir.x, cur.y - dir.y), COL_GRAY, 2.0f);
            char buf[64];
            if (s_handle == H_X) snprintf(buf, sizeof(buf), "X: %.2f", t.position.x - s_startPos.x);
            else if (s_handle == H_Y) snprintf(buf, sizeof(buf), "Y: %.2f", t.position.y - s_startPos.y);
            else snprintf(buf, sizeof(buf), "X: %.2f Y: %.2f", t.position.x - s_startPos.x, t.position.y - s_startPos.y);
            dl->AddText(ImVec2(cur.x + 15, cur.y + 15), IM_COL32(0, 0, 0, 255), buf);
            dl->AddText(ImVec2(cur.x + 14, cur.y + 14), IM_COL32(255, 255, 255, 255), buf);
        }
    } else if (mode == GizmoMode::Rotate) {
        // 圆环
        dl->AddCircle(center, R, ((s_drag && s_handle == H_ROT) || (!s_drag && hoverHandle == H_ROT)) ? COL_HL : COL_GRAY, 64, 2.0f);
        // 拖拽: 扇形(起始角→当前角) + 角度文字
        if (s_drag) {
            float a1 = atan2f(mousePos.y - center.y, mousePos.x - center.x);
            const int segs = 48;
            std::vector<ImVec2> pts;
            pts.reserve(segs + 2);
            pts.push_back(center);
            for (int i = 0; i <= segs; i++) {
                float a = s_startAngle + (a1 - s_startAngle) * i / segs;
                pts.push_back(ImVec2(center.x + cosf(a) * R, center.y + sinf(a) * R));
            }
            dl->AddConvexPolyFilled(pts.data(), (int)pts.size(), COL_FAN);
            glm::vec3 e = t.GetEulerAngles();
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f deg", e.z);
            dl->AddText(ImVec2(center.x + R + 14, center.y + R + 14), IM_COL32(0, 0, 0, 255), buf);
            dl->AddText(ImVec2(center.x + R + 13, center.y + R + 13), IM_COL32(255, 255, 255, 255), buf);
        }
    } else if (mode == GizmoMode::Scale) {
        bool hlX = (s_drag && s_handle == H_SX) || (!s_drag && hoverHandle == H_SX);
        bool hlY = (s_drag && s_handle == H_SY) || (!s_drag && hoverHandle == H_SY);
        bool hlC = (s_drag && s_handle == H_CENTER) || (!s_drag && hoverHandle == H_CENTER);
        dl->AddLine(center, ImVec2(center.x + R, center.y), COL_GRAY, 2.0f);
        dl->AddLine(center, ImVec2(center.x, center.y + R), COL_GRAY, 2.0f);
        dl->AddRectFilled(ImVec2(center.x + R - 6, center.y - 6), ImVec2(center.x + R + 6, center.y + 6), hlX ? COL_HL : COL_BLUE);
        dl->AddRectFilled(ImVec2(center.x - 6, center.y + R - 6), ImVec2(center.x + 6, center.y + R + 6), hlY ? COL_HL : COL_BLUE);
        // 中心方块: 两轴同时缩放
        dl->AddRectFilled(ImVec2(center.x - 6, center.y - 6), ImVec2(center.x + 6, center.y + 6), hlC ? COL_HL : COL_BLUE);
        if (s_drag) {
            char buf[64];
            snprintf(buf, sizeof(buf), "X: %.2f  Y: %.2f", t.scale.x, t.scale.y);
            ImVec2 pos(center.x + R + 14, center.y + R + 14);
            dl->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 255), buf);
            dl->AddText(pos, IM_COL32(255, 255, 255, 255), buf);
        }
    }
}

} // namespace Editor
