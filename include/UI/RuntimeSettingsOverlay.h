#pragma once

#include "Platform/Export.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

class Renderer2D;

namespace UI {

// 游戏内图形设置覆盖层。
// 这是引擎级 UI，不依赖具体游戏模块，因此桌面游戏模式和 Android
// 原型都能使用同一套入口与设置状态。
class MIKAN_API RuntimeSettingsOverlay {
public:
    static RuntimeSettingsOverlay& GetInstance();

    // 在主事件循环中调用。返回 true 表示事件被设置页消费，游戏输入不应再处理它。
    bool ProcessEvent(const SDL_Event& event);

    // 在后处理链末端的 UI pass 中调用，绘制设置按钮或打开的设置页。
    void Render(Renderer2D& renderer, int viewWidth, int viewHeight);

    bool IsOpen() const { return m_Open; }
    void SetOpen(bool open);

private:
    RuntimeSettingsOverlay() = default;
    RuntimeSettingsOverlay(const RuntimeSettingsOverlay&) = delete;
    RuntimeSettingsOverlay& operator=(const RuntimeSettingsOverlay&) = delete;

    struct Rect {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;

        bool Contains(glm::vec2 point) const {
            return point.x >= x && point.x <= x + w &&
                   point.y >= y && point.y <= y + h;
        }
    };

    Rect GearRect() const;
    Rect PanelRect() const;
    Rect ContentRect() const;
    Rect CloseRect() const;
    Rect RowRect(int row) const;
    Rect OpacitySliderRect() const;
    bool IsRowFullyVisible(int row) const;
    float MaxScrollOffset() const;
    void ScrollBy(float delta);
    void SetUIOpacityFromPosition(float x);
    bool HandleClick(glm::vec2 position);
    void TogglePass(const char* name);
    void ToggleGTAO();
    void ToggleBloom();
    bool ActivePassEnabled(const char* name) const;
    bool GTAOEnabled() const;
    bool BloomEnabled() const;

    int m_Width = 1;
    int m_Height = 1;
    bool m_Open = false;
    bool m_MousePressed = false;
    bool m_IgnoreMouseRelease = false;
    glm::vec2 m_MouseDownPosition = glm::vec2(0.0f);
    float m_MouseDownScrollOffset = 0.0f;
    bool m_MouseScrollCandidate = false;
    bool m_MouseDragging = false;
    bool m_MouseOpacityDragging = false;
    SDL_FingerID m_TouchFinger = static_cast<SDL_FingerID>(-1);
    bool m_IgnoreTouchRelease = false;
    glm::vec2 m_TouchDownPosition = glm::vec2(0.0f);
    float m_TouchDownScrollOffset = 0.0f;
    bool m_TouchScrollCandidate = false;
    bool m_TouchDragging = false;
    bool m_TouchOpacityDragging = false;
    float m_ScrollOffset = 0.0f;
};

} // namespace UI
