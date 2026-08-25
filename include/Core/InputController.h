#ifndef INPUTCONTROLLER_H
#define INPUTCONTROLLER_H
#include "Platform/Export.h"

#include <array>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include "Camera.h"
#include "VirtualJoystick.h"
#include "ECS/Types.h"
#include "World/World.h"

namespace ECS {
    class CameraComponent;
    class TransformComponent;
}

class Renderer2D;

class MIKAN_API InputController {
public:
    InputController();

    void ProcessInput(SDL_Event& event, Camera& camera, float deltaTime);
    void Update(Camera& camera, float deltaTime);
    void ProcessKeyboard(Camera& camera, float deltaTime);
    void ProcessTouch(Camera& camera, float deltaTime);
    void SetMouseCapture(bool capture);

    void RenderTouchControls();
    void RenderTouchControls(Renderer2D& renderer, int viewWidth, int viewHeight);

    glm::vec2 GetMousePosition() const;
    glm::vec2 GetMouseDelta() const;
    glm::vec2 ConsumeMouseDelta();
    float ConsumeMouseWheel();
    glm::vec4 GetMouseState() const;
    bool IsMouseButtonDown(int button) const;
    bool IsMouseButtonClicked(int button) const;
    bool IsMouseButtonPressed(int button) const;
    bool IsMouseButtonJustPressed(int button) const;
    bool IsKeyDown(SDL_Scancode scancode) const;

    bool IsMouseCaptured() const;
    // 方块交互（从 OpenGL 版迁移）：左键按住破坏、右键放置
    // currentBlockType 为手持方块类型（放置用）
    void HandleBlockInteraction(World& world, int currentBlockType, const World::HitResult& hit, float deltaTime);
    void SetBlockInteractionEnabled(bool enabled) { blockInteractionEnabled = enabled; }
    bool IsBlockInteractionEnabled() const { return blockInteractionEnabled; }

    void SetTouchEnabled(bool enabled);
    bool IsTouchEnabled() const;
    void SetWindow(SDL_Window* targetWindow);
    glm::vec2 GetTouchMoveDirection() const;
    glm::vec2 ConsumeTouchLookDelta();
    // 消费一次未被摇杆/动作键占用的短触摸，用于游戏内自绘按钮。
    bool ConsumeTouchTap(glm::vec2& outPosition);
    // 返回本帧双指间距变化（像素）：双指张开为正，合拢为负。
    float ConsumeTouchZoomDelta();

    enum class TouchAction : int {
        Attack = 0,
        Jump,
        Sprint,
        Crouch
    };

    bool IsTouchButtonDown(TouchAction action) const;
    bool ConsumeTouchButtonPressed(TouchAction action);
    
    glm::vec3 GetKeyboardMovementOffset(float deltaTime) const;
    
    void ProcessModeToggle();
    
    void UpdateSceneCamera(float deltaTime);
    void ProcessKeyboardForSceneCamera(float deltaTime, ECS::CameraComponent& camera, ECS::TransformComponent& transform);
    void ProcessMouseForSceneCamera(float deltaTime, ECS::CameraComponent& camera, ECS::TransformComponent& transform);
    void ProcessTouchForSceneCamera(float deltaTime, ECS::CameraComponent& camera, ECS::TransformComponent& transform);
    void FindSceneCamera();
    
    struct MIKAN_API JoystickConfig {
        float moveBaseRadius = 80.0f;
        float moveStickRadius = 50.0f;
        float moveMaxDistance = 60.0f;
        float moveOffsetX = 150.0f;
        float moveOffsetY = 150.0f;
        
        float lookBaseRadius = 80.0f;
        float lookStickRadius = 50.0f;
        float lookMaxDistance = 60.0f;
        float lookOffsetX = 0.0f;
        float lookOffsetY = 150.0f;
        
        float sensitivity = 0.5f;
        bool autoSave = true;
    };
    
    void SetJoystickConfig(const JoystickConfig& config);

private:
    void ProcessMouse(Camera& camera, float deltaTime, SDL_Event& event);
    void ProcessMouseButtons(SDL_Event& event);
    void UpdateWindowSize();
    void NormalizeMousePosition(float x, float y, float& outX, float& outY);
    void CenterMouse();
    void UpdateTouchButtonLayout(float width, float height);
    int FindTouchButton(float x, float y) const;
    void ResetTouchButtons();
    bool HandleTouchButtonDown(float x, float y, SDL_FingerID fingerId);
    bool HandleTouchButtonMotion(float x, float y, SDL_FingerID fingerId);
    bool HandleTouchButtonUp(SDL_FingerID fingerId);
    int FindTouchPoint(SDL_FingerID fingerId) const;
    void TrackTouchDown(SDL_FingerID fingerId, float x, float y, bool cameraEligible);
    void TrackTouchMotion(SDL_FingerID fingerId, float x, float y);
    void TrackTouchUp(SDL_FingerID fingerId);
    bool UpdateTouchPinchState();
    void ResetTouchTracking();
    static int TouchActionIndex(TouchAction action);

private:
    SDL_Window* window;
    bool mouseCaptured;
    bool firstMouse;
    float lastMouseX, lastMouseY;
    float currentMouseX, currentMouseY;
    float mouseDeltaX, mouseDeltaY;
    float mouseWheelDelta;
    float mouseSensitivity;

    bool leftButtonPressed;
    bool rightButtonPressed;
    bool leftButtonJustPressed;
    bool rightButtonJustPressed;
    bool leftButtonDown;
    bool rightButtonDown;
    bool leftButtonClicked;
    bool rightButtonClicked;

    bool leftButtonWasPressed;
    bool rightButtonWasPressed;

    glm::vec2 mouseClickPosition;
    int windowWidth, windowHeight;

    // Minecraft 风格的破坏/放置方块状态（从 OpenGL 版迁移）
    float leftButtonHoldTime = 0.0f;   // 左键按住时间
    float rightButtonHoldTime = 0.0f;  // 右键按住时间
    float breakCooldown = 0.0f;        // 破坏冷却
    float placeCooldown = 0.0f;        // 放置冷却
    bool isBreaking = false;           // 是否正在破坏
    glm::ivec3 breakingBlockPos = glm::ivec3(0); // 正在破坏的方块位置
    bool blockInteractionEnabled = true;         // 方块交互开关

    bool touchEnabled;
    VirtualJoystick moveJoystick;
    VirtualJoystick lookJoystick;
    bool touchActive;
    SDL_FingerID lookFingerId;
    float lastTouchX, lastTouchY;
    bool isTouchLooking;
    float touchLookDeltaX, touchLookDeltaY;
    struct TouchPoint {
        bool active = false;
        bool cameraEligible = false;
        SDL_FingerID fingerId = static_cast<SDL_FingerID>(-1);
        glm::vec2 position = glm::vec2(0.0f);
    };
    static constexpr int kTouchPointCount = 4;
    std::array<TouchPoint, kTouchPointCount> touchPoints;
    bool touchPinching;
    float touchPinchLastDistance;
    float touchZoomDelta;
    bool touchTapPending;
    bool touchTapCandidate;
    SDL_FingerID touchTapFingerId;
    glm::vec2 touchTapStartPosition;
    glm::vec2 touchTapPosition;

    static constexpr int kTouchActionCount = 4;
    std::array<glm::vec2, kTouchActionCount> touchButtonPositions;
    std::array<float, kTouchActionCount> touchButtonRadii;
    std::array<SDL_FingerID, kTouchActionCount> touchButtonFingerIds;
    std::array<bool, kTouchActionCount> touchButtonDown;
    std::array<bool, kTouchActionCount> touchButtonPressed;
    int touchMouseButtonIndex;
    
    ECS::Entity sceneCameraEntity;
    bool hasSceneCamera;
    float sceneCameraYaw;
    float sceneCameraPitch;
    
    bool sceneCameraTouchLooking;
    float lastSceneTouchX, lastSceneTouchY;
};

// 场景相机控制锁：游戏玩法脚本接管相机（如第三人称跟随）时置 true，
// 引擎 InputController::UpdateSceneCamera 直接跳过，不再用 WASD/鼠标驱动场景相机实体。
MIKAN_API void SetSceneCameraControlLocked(bool locked);
MIKAN_API bool IsSceneCameraControlLocked();

#endif
