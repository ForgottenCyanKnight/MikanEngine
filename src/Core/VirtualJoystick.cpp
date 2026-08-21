#include "VirtualJoystick.h"
#include "imgui.h"

VirtualJoystick::VirtualJoystick()
    : basePosition(100.0f, 300.0f)
    , stickPosition(100.0f, 300.0f)
    , direction(0.0f, 0.0f)
    , baseRadius(50.0f)
    , stickRadius(30.0f)
    , maxDistance(40.0f)
    , isActive(false)
    , isEnabled(true)
    , touchId(0)
    , fingerId(0)
{
}

void VirtualJoystick::Init(float baseRadius, float stickRadius, float maxDistance) {
    this->baseRadius = baseRadius;
    this->stickRadius = stickRadius;
    this->maxDistance = maxDistance;
}

void VirtualJoystick::SetPosition(float x, float y) {
    basePosition = glm::vec2(x, y);
    if (!isActive) {
        stickPosition = basePosition;
    }
}

void VirtualJoystick::SetEnabled(bool enabled) {
    isEnabled = enabled;
    if (!enabled) {
        isActive = false;
        direction = glm::vec2(0.0f, 0.0f);
    }
}

bool VirtualJoystick::IsPointInCircle(float px, float py, float cx, float cy, float radius) const {
    float dx = px - cx;
    float dy = py - cy;
    return (dx * dx + dy * dy) <= (radius * radius);
}

void VirtualJoystick::HandleTouch(SDL_Event& event) {
    if (!isEnabled) return;

    if (event.type == SDL_EVENT_FINGER_DOWN) {
        float x = event.tfinger.x * ImGui::GetIO().DisplaySize.x;
        float y = event.tfinger.y * ImGui::GetIO().DisplaySize.y;

        if (IsPointInCircle(x, y, basePosition.x, basePosition.y, baseRadius * 1.5f)) {
            isActive = true;
            touchId = event.tfinger.touchID;
            fingerId = event.tfinger.fingerID;
            stickPosition = glm::vec2(x, y);
            direction = glm::vec2(0.0f, 0.0f);
        }
    }
    else if (event.type == SDL_EVENT_FINGER_MOTION && isActive) {
        if (event.tfinger.touchID == touchId && event.tfinger.fingerID == fingerId) {
            float x = event.tfinger.x * ImGui::GetIO().DisplaySize.x;
            float y = event.tfinger.y * ImGui::GetIO().DisplaySize.y;

            glm::vec2 delta = glm::vec2(x, y) - basePosition;
            float dist = glm::length(delta);

            if (dist > maxDistance) {
                delta = glm::normalize(delta) * maxDistance;
            }

            stickPosition = basePosition + delta;
            direction = delta / maxDistance;
        }
    }
    else if (event.type == SDL_EVENT_FINGER_UP && isActive) {
        if (event.tfinger.touchID == touchId && event.tfinger.fingerID == fingerId) {
            isActive = false;
            stickPosition = basePosition;
            direction = glm::vec2(0.0f, 0.0f);
        }
    }
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        float x = (float)event.button.x;
        float y = (float)event.button.y;

        if (IsPointInCircle(x, y, basePosition.x, basePosition.y, baseRadius * 1.5f)) {
            isActive = true;
            touchId = 0;
            fingerId = 0;
            stickPosition = glm::vec2(x, y);
            direction = glm::vec2(0.0f, 0.0f);
        }
    }
    else if (event.type == SDL_EVENT_MOUSE_MOTION && isActive) {
        float x = (float)event.motion.x;
        float y = (float)event.motion.y;

        glm::vec2 delta = glm::vec2(x, y) - basePosition;
        float dist = glm::length(delta);

        if (dist > maxDistance) {
            delta = glm::normalize(delta) * maxDistance;
        }

        stickPosition = basePosition + delta;
        direction = delta / maxDistance;
    }
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && isActive) {
        isActive = false;
        stickPosition = basePosition;
        direction = glm::vec2(0.0f, 0.0f);
    }
}

glm::vec2 VirtualJoystick::GetDirection() const {
    return direction;
}

bool VirtualJoystick::IsActive() const {
    return isActive;
}

void VirtualJoystick::Render() {
    if (!isEnabled) return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    ImU32 baseColor = IM_COL32(80, 80, 80, 150);
    ImU32 stickColor = IM_COL32(150, 150, 150, 200);

    drawList->AddCircleFilled(ImVec2(basePosition.x, basePosition.y), baseRadius, baseColor);
    drawList->AddCircleFilled(ImVec2(stickPosition.x, stickPosition.y), stickRadius, stickColor);
}
