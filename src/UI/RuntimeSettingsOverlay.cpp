#include "UI/RuntimeSettingsOverlay.h"

#include "Core/InputGlobals.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TextRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace UI {

namespace {

constexpr int kRowCount = 8;
constexpr int kOpacityRow = kRowCount - 1;
constexpr float kMinUIOpacity = 0.2f;
constexpr float kMaxUIOpacity = 1.0f;
constexpr float kGearWidth = 116.0f;
constexpr float kGearHeight = 58.0f;
constexpr float kScreenMargin = 24.0f;
constexpr float kPanelMaxWidth = 760.0f;
constexpr float kPanelMaxHeight = 610.0f;
constexpr float kRowHeight = 58.0f;
constexpr float kRowGap = 10.0f;
constexpr float kContentTop = 92.0f;
constexpr float kContentBottomPadding = 24.0f;
constexpr float kScrollDragThreshold = 10.0f;

float RightSafeMargin(float width, float height)
{
#if defined(__ANDROID__)
    // Android 设备的右侧可能包含系统手势区、圆角或挖孔区域；
    // 设置入口不能贴着物理边缘绘制。按横屏 2400x1080 基准随窗口缩放，
    // 同时保留一个与屏幕宽度相关的下限，避免窄屏再次被裁剪。
    const float scale = std::clamp(height / 1080.0f, 0.70f, 1.0f);
    return std::clamp(std::max(192.0f * scale, width * 0.10f),
                      128.0f * scale, 320.0f * scale);
#else
    (void)width;
    (void)height;
    return kScreenMargin;
#endif
}

struct RowInfo {
    const char* label;
    const char* description;
};

constexpr RowInfo kRows[kRowCount] = {
    {"TAA", "时序抗锯齿"},
    {"GTAO", "屏幕空间环境光遮蔽"},
    {"Bloom", "全屏泛光链"},
    {"Tonemap", "HDR 色调映射"},
    {"FXAA", "快速抗锯齿"},
    {"VSync", "垂直同步"},
    {"Triple Buffer", "三重缓冲"},
    {"UI Opacity", "全局 UI 透明度"},
};

const glm::vec4 kTextPrimary(0.94f, 0.95f, 0.96f, 1.0f);
const glm::vec4 kTextSecondary(0.72f, 0.74f, 0.77f, 1.0f);
const glm::vec4 kPanelColor(0.14f, 0.15f, 0.17f, 0.92f);
const glm::vec4 kRowColor(0.25f, 0.26f, 0.28f, 0.78f);
const glm::vec4 kRowOnColor(0.38f, 0.40f, 0.43f, 0.88f);
const glm::vec4 kAccent(0.72f, 0.74f, 0.78f, 0.92f);

glm::vec4 ApplyUIOpacity(glm::vec4 color)
{
    color.a *= GetUIOpacity();
    return color;
}

void DrawText(Renderer2D& renderer, const std::string& text, float x, float top,
              float size, const glm::vec4& color, int layer)
{
    (void)renderer;
    TextRenderer& textRenderer = TextRenderer::GetInstance();
    if (!textRenderer.IsReady()) return;
    // TextRenderer 的 y 是 baseline；RuntimeSettingsOverlay 的布局 y 是顶部坐标。
    textRenderer.DrawStringMsdf(text, x, top + size * 0.82f, size,
                                ApplyUIOpacity(color), layer);
}

} // namespace

RuntimeSettingsOverlay& RuntimeSettingsOverlay::GetInstance()
{
    static RuntimeSettingsOverlay instance;
    return instance;
}

RuntimeSettingsOverlay::Rect RuntimeSettingsOverlay::GearRect() const
{
    const float rightSafeMargin = RightSafeMargin(
        static_cast<float>(m_Width), static_cast<float>(m_Height));
    return Rect{
        std::max(kScreenMargin, static_cast<float>(m_Width) - kGearWidth - rightSafeMargin),
        kScreenMargin,
        kGearWidth,
        kGearHeight};
}

RuntimeSettingsOverlay::Rect RuntimeSettingsOverlay::PanelRect() const
{
    const float width = std::min(kPanelMaxWidth,
                                 std::max(320.0f, static_cast<float>(m_Width) - 2.0f * kScreenMargin));
    const float height = std::min(kPanelMaxHeight,
                                  std::max(430.0f, static_cast<float>(m_Height) - 2.0f * kScreenMargin));
    return Rect{
        (static_cast<float>(m_Width) - width) * 0.5f,
        (static_cast<float>(m_Height) - height) * 0.5f,
        width,
        height};
}

RuntimeSettingsOverlay::Rect RuntimeSettingsOverlay::ContentRect() const
{
    const Rect panel = PanelRect();
    return Rect{
        panel.x + 20.0f,
        panel.y + kContentTop - 8.0f,
        panel.w - 40.0f,
        std::max(1.0f, panel.h - kContentTop - kContentBottomPadding + 8.0f)};
}

float RuntimeSettingsOverlay::MaxScrollOffset() const
{
    const Rect panel = PanelRect();
    const float contentHeight = static_cast<float>(kRowCount) * kRowHeight +
                                static_cast<float>(kRowCount - 1) * kRowGap;
    const float contentBottom = kContentTop + contentHeight;
    const float viewportBottom = panel.h - kContentBottomPadding;
    return std::max(0.0f, contentBottom - viewportBottom);
}

void RuntimeSettingsOverlay::ScrollBy(float delta)
{
    m_ScrollOffset = std::clamp(m_ScrollOffset + delta, 0.0f, MaxScrollOffset());
}

RuntimeSettingsOverlay::Rect RuntimeSettingsOverlay::CloseRect() const
{
    const Rect panel = PanelRect();
    return Rect{panel.x + panel.w - 126.0f, panel.y + 22.0f, 96.0f, 42.0f};
}

RuntimeSettingsOverlay::Rect RuntimeSettingsOverlay::RowRect(int row) const
{
    const Rect panel = PanelRect();
    return Rect{
        panel.x + 28.0f,
        panel.y + kContentTop - m_ScrollOffset +
            static_cast<float>(row) * (kRowHeight + kRowGap),
        panel.w - 56.0f,
        kRowHeight};
}

RuntimeSettingsOverlay::Rect RuntimeSettingsOverlay::OpacitySliderRect() const
{
    const Rect row = RowRect(kOpacityRow);
    // Keep the label/description on the left and leave room for the percentage
    // value on the right. The proportional fallback keeps the control usable
    // on narrow windows where the panel reaches its minimum width.
    const float sliderX = row.x + std::min(300.0f, row.w * 0.48f);
    const float sliderRight = row.x + row.w - 88.0f;
    return Rect{
        sliderX,
        row.y + 8.0f,
        std::max(48.0f, sliderRight - sliderX),
        row.h - 16.0f};
}

bool RuntimeSettingsOverlay::IsRowFullyVisible(int row) const
{
    const Rect rect = RowRect(row);
    const Rect content = ContentRect();
    return rect.y >= content.y && rect.y + rect.h <= content.y + content.h;
}

void RuntimeSettingsOverlay::SetUIOpacityFromPosition(float x)
{
    const Rect slider = OpacitySliderRect();
    if (slider.w <= 0.0f) return;
    const float normalized = std::clamp((x - slider.x) / slider.w, 0.0f, 1.0f);
    SetUIOpacity(kMinUIOpacity + normalized * (kMaxUIOpacity - kMinUIOpacity));
}

void RuntimeSettingsOverlay::SetOpen(bool open)
{
    if (m_Open == open) return;
    m_Open = open;
    m_MousePressed = false;
    m_IgnoreMouseRelease = false;
    m_MouseScrollCandidate = false;
    m_MouseDragging = false;
    m_MouseOpacityDragging = false;
    m_TouchFinger = static_cast<SDL_FingerID>(-1);
    m_IgnoreTouchRelease = false;
    m_TouchScrollCandidate = false;
    m_TouchDragging = false;
    m_TouchOpacityDragging = false;
    m_ScrollOffset = 0.0f;
}

bool RuntimeSettingsOverlay::ActivePassEnabled(const char* name) const
{
    const PostProcessChain* chain = nullptr;
    if (g_EditorActive && g_GameChain.IsBuilt()) {
        chain = &g_GameChain;
    } else if (g_SwapChain.IsBuilt()) {
        chain = &g_SwapChain;
    } else if (g_GameChain.IsBuilt()) {
        chain = &g_GameChain;
    }
    return chain != nullptr && chain->IsPassEnabled(name);
}

bool RuntimeSettingsOverlay::GTAOEnabled() const
{
    // gtao_apply consumes the generated AO texture. Treat the feature as on
    // only when both halves are enabled so the UI cannot report a broken pair.
    return ActivePassEnabled("gtao") && ActivePassEnabled("gtao_apply");
}

bool RuntimeSettingsOverlay::BloomEnabled() const
{
    return ActivePassEnabled("bloom_ds1");
}

void RuntimeSettingsOverlay::TogglePass(const char* name)
{
    const bool currentlyEnabled = ActivePassEnabled(name);
    const PostProcessChain* activeChain = nullptr;
    if (g_EditorActive && g_GameChain.IsBuilt()) {
        activeChain = &g_GameChain;
    } else if (g_SwapChain.IsBuilt()) {
        activeChain = &g_SwapChain;
    } else if (g_GameChain.IsBuilt()) {
        activeChain = &g_GameChain;
    }
    // 至少保留一个 pass 写入最终 framebuffer，避免用户把整条链全部关掉后
    // 最终交换链没有任何全屏绘制。
    if (currentlyEnabled && activeChain != nullptr && activeChain->GetEnabledPassCount() <= 1) return;

    const bool enabled = !currentlyEnabled;
    bool changed = false;
    changed |= g_SceneChain.SetPassEnabled(name, enabled);
    changed |= g_GameChain.SetPassEnabled(name, enabled);
    changed |= g_SwapChain.SetPassEnabled(name, enabled);
    if (changed) RequestPostProcessRebuild();
}

void RuntimeSettingsOverlay::ToggleGTAO()
{
    static constexpr std::array<const char*, 2> kGTAOPasses = {
        "gtao", "gtao_apply"};
    const bool currentlyEnabled = GTAOEnabled();
    const PostProcessChain* activeChain = nullptr;
    if (g_EditorActive && g_GameChain.IsBuilt()) {
        activeChain = &g_GameChain;
    } else if (g_SwapChain.IsBuilt()) {
        activeChain = &g_SwapChain;
    } else if (g_GameChain.IsBuilt()) {
        activeChain = &g_GameChain;
    }

    // Preserve at least one enabled full-screen pass when disabling the pair.
    if (currentlyEnabled && activeChain != nullptr &&
        activeChain->GetEnabledPassCount() <= static_cast<int>(kGTAOPasses.size())) {
        return;
    }

    const bool enabled = !currentlyEnabled;
    bool changed = false;
    for (const char* name : kGTAOPasses) {
        changed |= g_SceneChain.SetPassEnabled(name, enabled);
        changed |= g_GameChain.SetPassEnabled(name, enabled);
        changed |= g_SwapChain.SetPassEnabled(name, enabled);
    }
    if (changed) RequestPostProcessRebuild();
}

void RuntimeSettingsOverlay::ToggleBloom()
{
    static constexpr std::array<const char*, 11> kBloomPasses = {
        "bloom_ds1", "bloom_ds2", "bloom_ds3", "bloom_ds4", "bloom_ds5", "bloom_ds6",
        "bloom_up5", "bloom_up4", "bloom_up3", "bloom_up2", "bloom_up1"};
    const bool currentlyEnabled = BloomEnabled();
    const PostProcessChain* activeChain = nullptr;
    if (g_EditorActive && g_GameChain.IsBuilt()) {
        activeChain = &g_GameChain;
    } else if (g_SwapChain.IsBuilt()) {
        activeChain = &g_SwapChain;
    } else if (g_GameChain.IsBuilt()) {
        activeChain = &g_GameChain;
    }
    if (currentlyEnabled && activeChain != nullptr) {
        int activeBloomPasses = 0;
        for (const char* name : kBloomPasses) {
            if (activeChain->IsPassEnabled(name)) ++activeBloomPasses;
        }
        if (activeChain->GetEnabledPassCount() <= activeBloomPasses) return;
    }
    const bool enabled = !currentlyEnabled;
    bool changed = false;
    for (const char* name : kBloomPasses) {
        changed |= g_SceneChain.SetPassEnabled(name, enabled);
        changed |= g_GameChain.SetPassEnabled(name, enabled);
        changed |= g_SwapChain.SetPassEnabled(name, enabled);
    }
    if (changed) RequestPostProcessRebuild();
}

bool RuntimeSettingsOverlay::HandleClick(glm::vec2 position)
{
    if (!m_Open) {
        if (GearRect().Contains(position)) {
            SetOpen(true);
            return true;
        }
        return false;
    }

    if (CloseRect().Contains(position) || !PanelRect().Contains(position)) {
        SetOpen(false);
        return true;
    }

    for (int row = 0; row < kRowCount; ++row) {
        if (!RowRect(row).Contains(position)) continue;
        switch (row) {
        case 0: TogglePass("taa"); break;
        case 1: ToggleGTAO(); break;
        case 2: ToggleBloom(); break;
        case 3: TogglePass("tonemap"); break;
        case 4: TogglePass("fxaa"); break;
        case 5: SetVSync(!g_VSyncEnabled); break;
        case 6: SetTripleBuffering(!g_TripleBufferingEnabled); break;
        case kOpacityRow:
            if (IsRowFullyVisible(kOpacityRow) && OpacitySliderRect().Contains(position)) {
                SetUIOpacityFromPosition(position.x);
            }
            break;
        default: break;
        }
        return true;
    }
    return true;
}

bool RuntimeSettingsOverlay::ProcessEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_F10 || (m_Open && event.key.key == SDLK_ESCAPE)) {
            SetOpen(!m_Open);
            return true;
        }
        return m_Open;
    }
    if (event.type == SDL_EVENT_KEY_UP) return m_Open;

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        // SDL 默认会为触摸额外生成一套虚拟鼠标事件。触摸路径已经在
        // FINGER_* 分支处理过，若再次处理这里的 BUTTON_UP，会把打开面板
        // 的那次释放误判为“点面板外”，导致菜单刚打开就关闭。
        if (event.button.which == SDL_TOUCH_MOUSEID) return m_Open;
        if (event.button.button != SDL_BUTTON_LEFT) return m_Open;
        const glm::vec2 position(event.button.x, event.button.y);
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (!m_Open) {
                if (!GearRect().Contains(position)) return false;
                // 设置入口使用普通点按：按下时打开，后续释放只结束这次点按，
                // 不能被当成“点面板外”而立即关闭。
                SetOpen(true);
                m_IgnoreMouseRelease = true;
                return true;
            }
            m_MousePressed = true;
            m_MouseDownPosition = position;
            m_MouseDownScrollOffset = m_ScrollOffset;
            m_MouseOpacityDragging = IsRowFullyVisible(kOpacityRow) &&
                                     OpacitySliderRect().Contains(position);
            if (m_MouseOpacityDragging) {
                SetUIOpacityFromPosition(position.x);
                m_MouseScrollCandidate = false;
            } else {
                m_MouseScrollCandidate = ContentRect().Contains(position);
            }
            m_MouseDragging = false;
            return true;
        }
        if (m_IgnoreMouseRelease) {
            m_IgnoreMouseRelease = false;
            return true;
        }
        if (!m_MousePressed) return m_Open;
        if (m_MouseOpacityDragging) {
            m_MousePressed = false;
            m_MouseOpacityDragging = false;
            m_MouseScrollCandidate = false;
            return true;
        }
        if (m_MouseDragging) {
            m_MousePressed = false;
            m_MouseScrollCandidate = false;
            m_MouseDragging = false;
            return true;
        }
        m_MousePressed = false;
        m_MouseScrollCandidate = false;
        return HandleClick(position);
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        if (event.motion.which == SDL_TOUCH_MOUSEID) return m_Open;
        if (m_Open && m_MousePressed && m_MouseOpacityDragging) {
            SetUIOpacityFromPosition(event.motion.x);
            return m_Open;
        }
        if (m_Open && m_MousePressed && m_MouseScrollCandidate) {
            const glm::vec2 position(event.motion.x, event.motion.y);
            const float deltaY = position.y - m_MouseDownPosition.y;
            if (!m_MouseDragging && std::fabs(deltaY) >= kScrollDragThreshold) {
                m_MouseDragging = true;
            }
            if (m_MouseDragging) {
                ScrollBy(-deltaY - (m_ScrollOffset - m_MouseDownScrollOffset));
                m_MouseDownPosition.y = position.y;
                m_MouseDownScrollOffset = m_ScrollOffset;
            }
        }
        return m_Open;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        if (!m_Open) return false;
        ScrollBy(-event.wheel.y * 48.0f);
        return m_Open;
    }

    if (event.type == SDL_EVENT_FINGER_DOWN ||
        event.type == SDL_EVENT_FINGER_MOTION ||
        event.type == SDL_EVENT_FINGER_UP ||
        event.type == SDL_EVENT_FINGER_CANCELED) {
        const glm::vec2 position(
            event.tfinger.x * static_cast<float>(std::max(1, m_Width)),
            event.tfinger.y * static_cast<float>(std::max(1, m_Height)));
        const SDL_FingerID finger = event.tfinger.fingerID;

        if (event.type == SDL_EVENT_FINGER_DOWN) {
            if (!m_Open) {
                if (!GearRect().Contains(position)) return false;
                // 与鼠标入口一致：触摸按下立即打开，释放不关闭菜单。
                SetOpen(true);
                m_TouchFinger = finger;
                m_IgnoreTouchRelease = true;
                return true;
            }
            if (m_TouchFinger != static_cast<SDL_FingerID>(-1) && m_TouchFinger != finger) {
                return true;
            }
            m_TouchFinger = finger;
            m_TouchDownPosition = position;
            m_TouchDownScrollOffset = m_ScrollOffset;
            m_TouchOpacityDragging = IsRowFullyVisible(kOpacityRow) &&
                                     OpacitySliderRect().Contains(position);
            if (m_TouchOpacityDragging) {
                SetUIOpacityFromPosition(position.x);
                m_TouchScrollCandidate = false;
            } else {
                m_TouchScrollCandidate = ContentRect().Contains(position);
            }
            m_TouchDragging = false;
            return true;
        }
        if (!m_Open || m_TouchFinger != finger) return m_Open;
        if (event.type == SDL_EVENT_FINGER_MOTION) {
            if (m_TouchOpacityDragging) {
                SetUIOpacityFromPosition(position.x);
                return true;
            }
            if (m_TouchScrollCandidate) {
                const float deltaY = position.y - m_TouchDownPosition.y;
                if (!m_TouchDragging && std::fabs(deltaY) >= kScrollDragThreshold) {
                    m_TouchDragging = true;
                }
                if (m_TouchDragging) {
                    ScrollBy(m_TouchDownPosition.y - position.y -
                             (m_ScrollOffset - m_TouchDownScrollOffset));
                    m_TouchDownPosition.y = position.y;
                    m_TouchDownScrollOffset = m_ScrollOffset;
                }
            }
            return true;
        }

        m_TouchFinger = static_cast<SDL_FingerID>(-1);
        m_TouchScrollCandidate = false;
        if (event.type == SDL_EVENT_FINGER_CANCELED) {
            m_TouchDragging = false;
            m_TouchOpacityDragging = false;
            m_IgnoreTouchRelease = false;
            return true;
        }
        if (m_IgnoreTouchRelease) {
            m_IgnoreTouchRelease = false;
            m_TouchDragging = false;
            m_TouchOpacityDragging = false;
            return true;
        }
        if (m_TouchOpacityDragging) {
            m_TouchOpacityDragging = false;
            return true;
        }
        if (m_TouchDragging) {
            m_TouchDragging = false;
            return true;
        }
        return HandleClick(position);
    }

    return m_Open;
}

void RuntimeSettingsOverlay::Render(Renderer2D& renderer, int viewWidth, int viewHeight)
{
    if (viewWidth <= 0 || viewHeight <= 0) return;
    m_Width = viewWidth;
    m_Height = viewHeight;

    if (!m_Open) {
        const Rect gear = GearRect();
        renderer.DrawRect({gear.x + 2.0f, gear.y + 4.0f}, {gear.w, gear.h},
                          ApplyUIOpacity(glm::vec4(0.05f, 0.05f, 0.06f, 0.62f)), 200);
        renderer.DrawRect({gear.x, gear.y}, {gear.w, gear.h},
                          ApplyUIOpacity(glm::vec4(0.30f, 0.31f, 0.34f, 0.78f)), 201);
        DrawText(renderer, "设置", gear.x + 30.0f, gear.y + 13.0f, 26.0f, kTextPrimary, 202);
        return;
    }

    renderer.DrawRect({0.0f, 0.0f},
                      {static_cast<float>(m_Width), static_cast<float>(m_Height)},
                      ApplyUIOpacity(glm::vec4(0.03f, 0.03f, 0.04f, 0.52f)), 200);

    const Rect panel = PanelRect();
    renderer.DrawRect({panel.x + 5.0f, panel.y + 7.0f}, {panel.w, panel.h},
                      ApplyUIOpacity(glm::vec4(0.03f, 0.03f, 0.04f, 0.58f)), 201);
    renderer.DrawRect({panel.x, panel.y}, {panel.w, panel.h}, ApplyUIOpacity(kPanelColor), 202);
    renderer.DrawRect({panel.x, panel.y}, {panel.w, 6.0f}, ApplyUIOpacity(kAccent), 203);
    DrawText(renderer, "图形设置", panel.x + 28.0f, panel.y + 24.0f, 34.0f, kTextPrimary, 204);
    DrawText(renderer, "修改会在下一帧安全应用", panel.x + 30.0f, panel.y + 61.0f,
             18.0f, kTextSecondary, 204);

    const Rect close = CloseRect();
    renderer.DrawRect({close.x, close.y}, {close.w, close.h},
                      ApplyUIOpacity(glm::vec4(0.34f, 0.35f, 0.38f, 0.84f)), 204);
    DrawText(renderer, "关闭", close.x + 23.0f, close.y + 8.0f, 22.0f, kTextPrimary, 205);

    for (int row = 0; row < kRowCount; ++row) {
        const Rect rect = RowRect(row);
        const Rect content = ContentRect();
        // Renderer2D 没有独立 scissor；只绘制完整可见的行，避免滚动时
        // 文本或行背景穿出标题/面板边界。
        if (rect.y < content.y || rect.y + rect.h > content.y + content.h) continue;
        bool enabled = false;
        switch (row) {
        case 0: enabled = ActivePassEnabled("taa"); break;
        case 1: enabled = GTAOEnabled(); break;
        case 2: enabled = BloomEnabled(); break;
        case 3: enabled = ActivePassEnabled("tonemap"); break;
        case 4: enabled = ActivePassEnabled("fxaa"); break;
        case 5: enabled = g_VSyncEnabled; break;
        case 6: enabled = g_TripleBufferingEnabled; break;
        default: break;
        }
        const bool isOpacityRow = row == kOpacityRow;
        renderer.DrawRect({rect.x, rect.y}, {rect.w, rect.h},
                          ApplyUIOpacity(isOpacityRow ? kRowOnColor
                                                       : (enabled ? kRowOnColor : kRowColor)), 204);
        DrawText(renderer, kRows[row].label, rect.x + 20.0f, rect.y + 8.0f,
                 23.0f, kTextPrimary, 205);
        DrawText(renderer, kRows[row].description, rect.x + 20.0f, rect.y + 34.0f,
                 15.0f, kTextSecondary, 205);

        if (isOpacityRow) {
            const Rect slider = OpacitySliderRect();
            const float normalized = std::clamp(
                (GetUIOpacity() - kMinUIOpacity) / (kMaxUIOpacity - kMinUIOpacity),
                0.0f, 1.0f);
            const float trackY = rect.y + (rect.h - 8.0f) * 0.5f;
            renderer.DrawRect({slider.x, trackY}, {slider.w, 8.0f},
                              ApplyUIOpacity(glm::vec4(0.12f, 0.13f, 0.14f, 0.90f)), 206);
            renderer.DrawRect({slider.x, trackY}, {slider.w * normalized, 8.0f},
                              ApplyUIOpacity(glm::vec4(0.68f, 0.70f, 0.74f, 0.92f)), 207);
            const float thumbWidth = 16.0f;
            const float thumbX = std::clamp(slider.x + slider.w * normalized - thumbWidth * 0.5f,
                                            slider.x, slider.x + slider.w - thumbWidth);
            renderer.DrawRect({thumbX, rect.y + 12.0f}, {thumbWidth, 34.0f},
                              ApplyUIOpacity(glm::vec4(0.88f, 0.89f, 0.91f, 0.96f)), 208);

            char value[16] = {};
            std::snprintf(value, sizeof(value), "%d%%",
                          static_cast<int>(std::lround(GetUIOpacity() * 100.0f)));
            const float valueWidth = TextRenderer::GetInstance().MeasureString(value, 19.0f);
            DrawText(renderer, value, rect.x + rect.w - valueWidth - 18.0f,
                     rect.y + 18.0f, 19.0f, kTextPrimary, 209);
            continue;
        }

        const float toggleWidth = 112.0f;
        const float toggleHeight = 38.0f;
        const float toggleX = rect.x + rect.w - toggleWidth - 16.0f;
        const float toggleY = rect.y + (rect.h - toggleHeight) * 0.5f;
        renderer.DrawRect({toggleX, toggleY}, {toggleWidth, toggleHeight},
                          ApplyUIOpacity(enabled ? glm::vec4(0.56f, 0.58f, 0.61f, 0.90f)
                                                 : glm::vec4(0.24f, 0.25f, 0.27f, 0.82f)), 206);
        const char* state = enabled ? "开启" : "关闭";
        const float stateWidth = TextRenderer::GetInstance().MeasureString(state, 19.0f);
        DrawText(renderer, state, toggleX + (toggleWidth - stateWidth) * 0.5f,
                 toggleY + 7.0f, 19.0f, kTextPrimary, 207);
    }

    const float maxScroll = MaxScrollOffset();
    if (maxScroll > 0.0f) {
        const Rect content = ContentRect();
        const float trackWidth = 6.0f;
        const float thumbHeight = std::max(24.0f,
            content.h * content.h / (content.h + maxScroll));
        const float thumbTravel = std::max(0.0f, content.h - thumbHeight);
        const float thumbY = content.y + thumbTravel * (m_ScrollOffset / maxScroll);
        renderer.DrawRect({panel.x + panel.w - 14.0f, content.y},
                          {trackWidth, content.h},
                          ApplyUIOpacity(glm::vec4(0.12f, 0.13f, 0.14f, 0.52f)), 210);
        renderer.DrawRect({panel.x + panel.w - 14.0f, thumbY},
                          {trackWidth, thumbHeight},
                          ApplyUIOpacity(glm::vec4(0.66f, 0.68f, 0.71f, 0.82f)), 211);
    }
}

} // namespace UI
