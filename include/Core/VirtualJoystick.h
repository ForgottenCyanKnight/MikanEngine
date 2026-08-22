#ifndef VIRTUALJOYSTICK_H
#define VIRTUALJOYSTICK_H

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

class VirtualJoystick {
public:
    VirtualJoystick();

    void Init(float baseRadius, float stickRadius, float maxDistance);
    void Render();
    void HandleTouch(SDL_Event& event);
    void SetScreenSize(float width, float height);

    glm::vec2 GetDirection() const;
    bool IsActive() const;
    glm::vec2 GetPosition() const { return basePosition; }
    glm::vec2 GetStickPosition() const { return stickPosition; }
    float GetBaseRadius() const { return baseRadius; }
    float GetStickRadius() const { return stickRadius; }

    void SetPosition(float x, float y);
    void SetEnabled(bool enabled);
    bool IsPointInCircle(float px, float py, float cx, float cy, float radius) const;

private:
    glm::vec2 basePosition;
    glm::vec2 stickPosition;
    glm::vec2 direction;
    float baseRadius;
    float stickRadius;
    float maxDistance;
    bool isActive;
    bool isEnabled;
    SDL_TouchID touchId;
    SDL_FingerID fingerId;
    float screenWidth;
    float screenHeight;
};

#endif
