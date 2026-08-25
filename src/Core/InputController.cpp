#include "InputController.h"
#include "imgui.h"
#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "EngineGlobal.h"
#include "Core/RenderGlobals.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "Core/EngineConfig.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TextRenderer.h"
#include <algorithm>
#include <cmath>

namespace {
    constexpr SDL_FingerID kTouchMouseFingerId = static_cast<SDL_FingerID>(-2);

    float TouchRightSafeInset(float width, float height) {
#if defined(__ANDROID__)
        // 绘制和命中共用同一安全边距，避免右侧系统手势区/圆角裁剪操作键。
        const float scale = std::clamp(height / 1080.0f, 0.70f, 1.0f);
        return std::clamp(std::max(192.0f * scale, width * 0.10f),
                          128.0f * scale, 320.0f * scale);
#else
        (void)width;
        (void)height;
        return 96.0f;
#endif
    }

    glm::vec4 ApplyUIOpacity(glm::vec4 color) {
        color.a *= GetUIOpacity();
        return color;
    }
}

InputController::InputController()
    : window(nullptr), mouseCaptured(false), firstMouse(true),
    lastMouseX(0.0f), lastMouseY(0.0f),
    currentMouseX(0.0f), currentMouseY(0.0f),
    mouseDeltaX(0.0f), mouseDeltaY(0.0f),
    mouseWheelDelta(0.0f),
    mouseSensitivity(0.5f),
    leftButtonPressed(false), rightButtonPressed(false),
    leftButtonJustPressed(false), rightButtonJustPressed(false),
    leftButtonDown(false), rightButtonDown(false),
    leftButtonClicked(false), rightButtonClicked(false),
    leftButtonWasPressed(false), rightButtonWasPressed(false),
    mouseClickPosition(0.0f, 0.0f),
    windowWidth(1280 * 2), windowHeight(1280),
    touchEnabled(false), touchActive(false), lastTouchX(0), lastTouchY(0), isTouchLooking(false), lookFingerId(0),
    touchLookDeltaX(0.0f), touchLookDeltaY(0.0f),
    touchPinching(false), touchPinchLastDistance(0.0f), touchZoomDelta(0.0f),
    touchTapPending(false), touchTapCandidate(false),
    touchTapFingerId(static_cast<SDL_FingerID>(-1)),
    touchTapStartPosition(0.0f), touchTapPosition(0.0f),
    touchMouseButtonIndex(-1),
    sceneCameraEntity(ECS::INVALID_ENTITY), hasSceneCamera(false),
    sceneCameraYaw(-90.0f), sceneCameraPitch(0.0f),
    sceneCameraTouchLooking(false), lastSceneTouchX(0), lastSceneTouchY(0)
{
    moveJoystick.Init(80.0f, 50.0f, 60.0f);
    lookJoystick.Init(80.0f, 50.0f, 60.0f);
    touchButtonFingerIds.fill(static_cast<SDL_FingerID>(-1));
    touchButtonDown.fill(false);
    touchButtonPressed.fill(false);
    ResetTouchTracking();
    UpdateTouchButtonLayout(static_cast<float>(windowWidth), static_cast<float>(windowHeight));
}

// 场景相机控制锁：玩家脚本接管相机时置 true（见 InputController.h 的 Set/Is 接口）
static bool sSceneCameraControlLocked = false;
void SetSceneCameraControlLocked(bool locked) { sSceneCameraControlLocked = locked; }
bool IsSceneCameraControlLocked() { return sSceneCameraControlLocked; }

void InputController::CenterMouse() {
    if (window && mouseCaptured) {
        int centerX = windowWidth / 2;
        int centerY = windowHeight / 2;
        SDL_WarpMouseInWindow(window, centerX, centerY);
    }
}

void InputController::ProcessInput(SDL_Event& event, Camera& camera, float deltaTime) {
    UpdateWindowSize();

    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        mouseWheelDelta += event.wheel.y;
    }

    if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION || event.type == SDL_EVENT_FINGER_UP) {
        if (touchEnabled) {
            const float screenWidth = windowWidth > 0 ? static_cast<float>(windowWidth) : 1.0f;
            const float screenHeight = windowHeight > 0 ? static_cast<float>(windowHeight) : 1.0f;
            float x = event.tfinger.x * screenWidth;
            float y = event.tfinger.y * screenHeight;
            const SDL_FingerID fingerId = event.tfinger.fingerID;
            
            if (event.type == SDL_EVENT_FINGER_DOWN) {
                const bool inMoveJoystick = moveJoystick.IsPointInCircle(
                    x, y, moveJoystick.GetPosition().x, moveJoystick.GetPosition().y,
                    moveJoystick.GetBaseRadius() * 1.5f);
                if (inMoveJoystick) {
                    TrackTouchDown(fingerId, x, y, false);
                    moveJoystick.HandleTouch(event);
                } else if (HandleTouchButtonDown(x, y, fingerId)) {
                    TrackTouchDown(fingerId, x, y, false);
                } else {
                    TrackTouchDown(fingerId, x, y, true);
                    const bool pinchActive = UpdateTouchPinchState();
                    if (!pinchActive) {
                        touchTapCandidate = true;
                        touchTapFingerId = fingerId;
                        touchTapStartPosition = glm::vec2(x, y);
                        lastTouchX = x;
                        lastTouchY = y;
                        isTouchLooking = true;
                        lookFingerId = fingerId;
                        touchLookDeltaX = 0.0f;
                        touchLookDeltaY = 0.0f;
                    }
                }
            } else if (event.type == SDL_EVENT_FINGER_MOTION) {
                TrackTouchMotion(fingerId, x, y);
                if (touchTapCandidate && touchTapFingerId == fingerId &&
                    glm::distance(touchTapStartPosition, glm::vec2(x, y)) > 24.0f) {
                    touchTapCandidate = false;
                }
                const bool pinchActive = UpdateTouchPinchState();
                const int touchPointIndex = FindTouchPoint(fingerId);
                const bool cameraFinger = touchPointIndex >= 0 &&
                    touchPoints[static_cast<size_t>(touchPointIndex)].cameraEligible;

                if (pinchActive && cameraFinger) {
                    // 双指状态只调整相机距离，不再把两根手指的移动量当作转视角。
                } else if (isTouchLooking && fingerId == lookFingerId) {
                    float deltaX = x - lastTouchX;
                    float deltaY = y - lastTouchY;
                    
                    const float maxDelta = 50.0f;
                    deltaX = glm::clamp(deltaX, -maxDelta, maxDelta);
                    deltaY = glm::clamp(deltaY, -maxDelta, maxDelta);
                    
                    // 累加增量值
                    touchLookDeltaX += deltaX;
                    touchLookDeltaY += deltaY;
                    
                    lastTouchX = x;
                    lastTouchY = y;
                } else if (HandleTouchButtonMotion(x, y, fingerId)) {
                    // Button fingers are consumed by the action control, not the camera.
                } else {
                    moveJoystick.HandleTouch(event);
                }
            } else if (event.type == SDL_EVENT_FINGER_UP) {
                if (touchTapCandidate && touchTapFingerId == fingerId) {
                    const glm::vec2 releasePosition(x, y);
                    if (glm::distance(touchTapStartPosition, releasePosition) <= 24.0f) {
                        touchTapPending = true;
                        touchTapPosition = releasePosition;
                    }
                    touchTapCandidate = false;
                    touchTapFingerId = static_cast<SDL_FingerID>(-1);
                }
                const int touchPointIndex = FindTouchPoint(fingerId);
                const bool cameraFinger = touchPointIndex >= 0 &&
                    touchPoints[static_cast<size_t>(touchPointIndex)].cameraEligible;
                const bool wasPinching = touchPinching && cameraFinger;

                if (wasPinching) {
                    TrackTouchUp(fingerId);
                    UpdateTouchPinchState();
                } else if (isTouchLooking && fingerId == lookFingerId) {
                    isTouchLooking = false;
                    TrackTouchUp(fingerId);
                    UpdateTouchPinchState();
                } else if (HandleTouchButtonUp(fingerId)) {
                    // Button release is handled independently from the joystick.
                    TrackTouchUp(fingerId);
                } else {
                    moveJoystick.HandleTouch(event);
                    TrackTouchUp(fingerId);
                    UpdateTouchPinchState();
                }
            }
        }
    }

    if (touchEnabled && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
        const bool isMouseMotion = event.type == SDL_EVENT_MOUSE_MOTION;
        float x = isMouseMotion ? event.motion.x : event.button.x;
        float y = isMouseMotion ? event.motion.y : event.button.y;
        
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (moveJoystick.IsPointInCircle(x, y, moveJoystick.GetPosition().x, moveJoystick.GetPosition().y, moveJoystick.GetBaseRadius() * 1.5f)) {
                moveJoystick.HandleTouch(event);
            } else {
                const int buttonIndex = FindTouchButton(x, y);
                if (buttonIndex >= 0) {
                    // Some SDL backends synthesize mouse events for a finger.  Do not
                    // replace the real finger owner when that happens.
                    if (!touchButtonDown[buttonIndex]) {
                        touchButtonDown[buttonIndex] = true;
                        touchButtonPressed[buttonIndex] = true;
                        touchButtonFingerIds[buttonIndex] = kTouchMouseFingerId;
                        touchMouseButtonIndex = buttonIndex;
                    }
                } else {
                    touchTapCandidate = true;
                    touchTapFingerId = kTouchMouseFingerId;
                    touchTapStartPosition = glm::vec2(x, y);
                    lastTouchX = x;
                    lastTouchY = y;
                    isTouchLooking = true;
                    touchLookDeltaX = 0.0f;
                    touchLookDeltaY = 0.0f;
                }
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            if (isTouchLooking) {
                float deltaX = x - lastTouchX;
                float deltaY = y - lastTouchY;

                if (touchTapCandidate && touchTapFingerId == kTouchMouseFingerId &&
                    glm::distance(touchTapStartPosition, glm::vec2(x, y)) > 24.0f) {
                    touchTapCandidate = false;
                }
                
                const float maxDelta = 50.0f;
                deltaX = glm::clamp(deltaX, -maxDelta, maxDelta);
                deltaY = glm::clamp(deltaY, -maxDelta, maxDelta);
                
                // 累加增量值
                touchLookDeltaX += deltaX;
                touchLookDeltaY += deltaY;
                
                lastTouchX = x;
                lastTouchY = y;
            } else if (touchMouseButtonIndex >= 0) {
                const int buttonIndex = FindTouchButton(x, y);
                if (buttonIndex != touchMouseButtonIndex) {
                    touchButtonDown[touchMouseButtonIndex] = false;
                    touchButtonFingerIds[touchMouseButtonIndex] = static_cast<SDL_FingerID>(-1);
                }
            } else {
                moveJoystick.HandleTouch(event);
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (isTouchLooking) {
                if (touchTapCandidate && touchTapFingerId == kTouchMouseFingerId &&
                    glm::distance(touchTapStartPosition, glm::vec2(x, y)) <= 24.0f) {
                    touchTapPending = true;
                    touchTapPosition = glm::vec2(x, y);
                }
                touchTapCandidate = false;
                touchTapFingerId = static_cast<SDL_FingerID>(-1);
                isTouchLooking = false;
            } else if (touchMouseButtonIndex >= 0) {
                touchButtonDown[touchMouseButtonIndex] = false;
                touchButtonFingerIds[touchMouseButtonIndex] = static_cast<SDL_FingerID>(-1);
                touchMouseButtonIndex = -1;
            } else {
                moveJoystick.HandleTouch(event);
            }
        }
    }

    if (!touchEnabled && event.type == SDL_EVENT_MOUSE_MOTION) {
        ProcessMouse(camera, deltaTime, event);
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_TAB) {
        SetMouseCapture(!mouseCaptured);
    }

    if (!touchEnabled && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
        ProcessMouseButtons(event);
    }
}

void InputController::Update(Camera& camera, float deltaTime) {
    // 只在窗口大小变化时更新，避免每帧调用
    static int lastWindowWidth = 0;
    static int lastWindowHeight = 0;
    if (windowWidth != lastWindowWidth || windowHeight != lastWindowHeight) {
        UpdateWindowSize();
        lastWindowWidth = windowWidth;
        lastWindowHeight = windowHeight;
    }
    
    ProcessKeyboard(camera, deltaTime);

    if (touchEnabled) {
        ProcessTouch(camera, deltaTime);
    }

    leftButtonJustPressed = (leftButtonPressed && !leftButtonWasPressed);
    rightButtonJustPressed = (rightButtonPressed && !rightButtonWasPressed);

    leftButtonDown = leftButtonPressed;
    rightButtonDown = rightButtonPressed;
    leftButtonClicked = leftButtonJustPressed;
    rightButtonClicked = rightButtonJustPressed;

    if (leftButtonJustPressed || rightButtonJustPressed) {
        mouseClickPosition = glm::vec2(currentMouseX, currentMouseY);
    }

    leftButtonWasPressed = leftButtonPressed;
    rightButtonWasPressed = rightButtonPressed;
}

void InputController::HandleBlockInteraction(World& world, int currentBlockType, const World::HitResult& hit, float deltaTime) {
    // 方块交互开关
    if (!blockInteractionEnabled) {
        isBreaking = false;
        leftButtonHoldTime = 0.0f;
        return;
    }

    // 更新冷却时间
    if (breakCooldown > 0.0f) {
        breakCooldown -= deltaTime;
        if (breakCooldown < 0.0f) breakCooldown = 0.0f;
    }
    if (placeCooldown > 0.0f) {
        placeCooldown -= deltaTime;
        if (placeCooldown < 0.0f) placeCooldown = 0.0f;
    }

    // Minecraft 风格的破坏逻辑（左键按住）
    if (leftButtonPressed && hit.hit) {
        // 检查是否开始破坏新的方块
        if (!isBreaking || breakingBlockPos != hit.position) {
            isBreaking = true;
            breakingBlockPos = hit.position;
            leftButtonHoldTime = 0.0f; // 重置破坏计时器
        }

        // 模拟 Minecraft 的破坏进度
        const float breakTime = 0.1f;
        leftButtonHoldTime += deltaTime;

        // 破坏时间达到且冷却结束，执行破坏
        if (leftButtonHoldTime >= breakTime && breakCooldown <= 0.0f) {
            world.PlaceBlock(breakingBlockPos, 0);
            breakCooldown = 0.1f;
            leftButtonHoldTime = 0.0f;
        }
    } else {
        // 左键未按下或未击中方块时停止破坏
        isBreaking = false;
    }

    // Minecraft 风格的放置逻辑（右键）
    if (rightButtonPressed && hit.hit && placeCooldown <= 0.0f) {
        // 右键放置：在命中面的外侧放置
        world.PlaceBlock(hit.position + hit.normal, currentBlockType);
        placeCooldown = 0.2f; // 放置后冷却 0.2 秒
    }
}

void InputController::ProcessKeyboard(Camera& camera, float deltaTime) {
    const bool* keyState = SDL_GetKeyboardState(NULL);

    if (keyState[SDL_SCANCODE_W]) camera.ProcessKeyboard(0, deltaTime);
    if (keyState[SDL_SCANCODE_S]) camera.ProcessKeyboard(1, deltaTime);
    if (keyState[SDL_SCANCODE_A]) camera.ProcessKeyboard(2, deltaTime);
    if (keyState[SDL_SCANCODE_D]) camera.ProcessKeyboard(3, deltaTime);
    if (keyState[SDL_SCANCODE_SPACE]) camera.ProcessKeyboard(4, deltaTime);
    if (keyState[SDL_SCANCODE_LSHIFT] || keyState[SDL_SCANCODE_RSHIFT])
        camera.ProcessKeyboard(5, deltaTime);
}

void InputController::ProcessTouch(Camera& camera, float deltaTime) {
    glm::vec2 moveDir = moveJoystick.GetDirection();
    glm::vec2 lookDir = lookJoystick.GetDirection();

    if (glm::length(moveDir) > 0.1f) {
        if (moveDir.y < -0.3f) camera.ProcessKeyboard(0, deltaTime);
        if (moveDir.y > 0.3f) camera.ProcessKeyboard(1, deltaTime);
        if (moveDir.x < -0.3f) camera.ProcessKeyboard(2, deltaTime);
        if (moveDir.x > 0.3f) camera.ProcessKeyboard(3, deltaTime);
    }

    if (glm::length(lookDir) > 0.1f) {
        float xoffset = lookDir.x * 50.0f * deltaTime;
        float yoffset = lookDir.y * 50.0f * deltaTime;
        camera.ProcessMouseMovement(xoffset, yoffset);
    }
}

void InputController::RenderTouchControls() {
    if (!touchEnabled) return;

    moveJoystick.Render();
    lookJoystick.Render();
}

void InputController::RenderTouchControls(Renderer2D& renderer, int viewWidth, int viewHeight) {
    if (!touchEnabled || viewWidth <= 0 || viewHeight <= 0) return;

    moveJoystick.SetScreenSize(static_cast<float>(viewWidth), static_cast<float>(viewHeight));
    UpdateTouchButtonLayout(static_cast<float>(viewWidth), static_cast<float>(viewHeight));

    // Renderer2D 的基础图元是四边形；用三角扇退化四边形绘制低开销圆形，
    // 这样纯安卓运行时不需要创建 ImGui context 也能看到真实摇杆。
    const auto drawCircle = [&](const glm::vec2& center, float radius,
                                const glm::vec4& color, int layer) {
        if (radius <= 0.0f) return;
        constexpr int kSegments = 24;
        const float step = 6.28318530718f / static_cast<float>(kSegments);
        Quad2D quad;
        quad.uv0 = glm::vec2(0.0f);
        quad.uv1 = glm::vec2(1.0f);
        quad.color = color;
        quad.texture = renderer.GetWhiteTexture();
        quad.layer = layer;
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = step * static_cast<float>(i);
            const float a1 = step * static_cast<float>(i + 1);
            quad.p0 = center;
            quad.p1 = center + glm::vec2(std::cos(a0), std::sin(a0)) * radius;
            quad.p2 = center + glm::vec2(std::cos(a1), std::sin(a1)) * radius;
            quad.p3 = quad.p2;
            renderer.DrawQuad(quad);
        }
    };

    const glm::vec2 base = moveJoystick.GetPosition();
    const glm::vec2 stick = moveJoystick.GetStickPosition();
    const float baseRadius = moveJoystick.GetBaseRadius();
    const float stickRadius = moveJoystick.GetStickRadius();
    drawCircle(base, baseRadius + 5.0f,
               ApplyUIOpacity(glm::vec4(0.06f, 0.06f, 0.07f, 0.64f)), 90);
    drawCircle(base, baseRadius,
               ApplyUIOpacity(glm::vec4(0.32f, 0.33f, 0.35f, 0.46f)), 91);
    drawCircle(stick, stickRadius, moveJoystick.IsActive()
        ? ApplyUIOpacity(glm::vec4(0.78f, 0.80f, 0.83f, 0.84f))
        : ApplyUIOpacity(glm::vec4(0.56f, 0.58f, 0.61f, 0.62f)), 92);

    static constexpr const char* kTouchButtonTextureNames[] = {
        "touch_action_attack",
        "touch_action_jump",
        "touch_action_sprint",
        "touch_action_crouch"
    };
    static constexpr const char* kTouchButtonTexturePaths[] = {
        "ui/actions/touch_attack.png",
        "ui/actions/touch_jump.png",
        "ui/actions/touch_sprint.png",
        "ui/actions/touch_crouch.png"
    };
    static const glm::vec4 kTouchButtonColor(0.42f, 0.44f, 0.47f, 0.76f);
    const glm::vec4 pressedColor(0.76f, 0.78f, 0.81f, 0.92f);
    std::array<bool, kTouchActionCount> touchButtonIconsLoaded{};
    for (int i = 0; i < kTouchActionCount; ++i) {
        const glm::vec2& position = touchButtonPositions[static_cast<size_t>(i)];
        const float radius = touchButtonRadii[static_cast<size_t>(i)];
        drawCircle(position, radius + 5.0f,
                   ApplyUIOpacity(glm::vec4(0.06f, 0.06f, 0.07f, 0.68f)), 94);
        drawCircle(position, radius,
                   ApplyUIOpacity(touchButtonDown[static_cast<size_t>(i)]
                                      ? pressedColor : kTouchButtonColor), 95);

        // Load lazily here so the same path works for desktop assets and Android APK assets.
        // LoadTexture is idempotent and also repopulates the descriptor after a renderer reset.
        touchButtonIconsLoaded[static_cast<size_t>(i)] = renderer.LoadTexture(
            kTouchButtonTextureNames[i], EngineConfig::GetFullPath(kTouchButtonTexturePaths[i]));
        if (touchButtonIconsLoaded[static_cast<size_t>(i)]) {
            const float iconSize = radius * 0.78f;
            const glm::vec4 iconColor = touchButtonDown[static_cast<size_t>(i)]
                ? glm::vec4(1.0f, 1.0f, 1.0f, 0.98f)
                : glm::vec4(0.94f, 0.95f, 0.97f, 0.92f);
            renderer.DrawSprite(position - glm::vec2(iconSize * 0.5f),
                                 glm::vec2(iconSize),
                                 renderer.GetTexture(kTouchButtonTextureNames[i]),
                                 glm::vec2(0.0f), glm::vec2(1.0f),
                                 ApplyUIOpacity(iconColor), 96);
        }
    }
}

void InputController::SetTouchEnabled(bool enabled) {
    touchEnabled = enabled;
    moveJoystick.SetEnabled(enabled);
    lookJoystick.SetEnabled(false);

    if (!enabled) {
        isTouchLooking = false;
        touchLookDeltaX = 0.0f;
        touchLookDeltaY = 0.0f;
        ResetTouchTracking();
        ResetTouchButtons();
    }

    if (enabled) {
        UpdateWindowSize();
        float offsetX = 150.0f;
        float offsetY = windowHeight - 150.0f;
        moveJoystick.SetPosition(offsetX, offsetY);
        UpdateTouchButtonLayout(static_cast<float>(windowWidth), static_cast<float>(windowHeight));
    }
}

bool InputController::IsTouchEnabled() const {
    return touchEnabled;
}

void InputController::SetWindow(SDL_Window* targetWindow) {
    if (!targetWindow) return;
    window = targetWindow;
    UpdateWindowSize();
}

glm::vec2 InputController::GetTouchMoveDirection() const {
    return touchEnabled ? moveJoystick.GetDirection() : glm::vec2(0.0f);
}

int InputController::FindTouchPoint(SDL_FingerID fingerId) const {
    for (int i = 0; i < kTouchPointCount; ++i) {
        const TouchPoint& point = touchPoints[static_cast<size_t>(i)];
        if (point.active && point.fingerId == fingerId) return i;
    }
    return -1;
}

void InputController::TrackTouchDown(SDL_FingerID fingerId, float x, float y,
                                     bool cameraEligible) {
    int index = FindTouchPoint(fingerId);
    if (index < 0) {
        for (int i = 0; i < kTouchPointCount; ++i) {
            if (!touchPoints[static_cast<size_t>(i)].active) {
                index = i;
                break;
            }
        }
    }
    if (index < 0) return;

    TouchPoint& point = touchPoints[static_cast<size_t>(index)];
    point.active = true;
    point.cameraEligible = cameraEligible;
    point.fingerId = fingerId;
    point.position = glm::vec2(x, y);
}

void InputController::TrackTouchMotion(SDL_FingerID fingerId, float x, float y) {
    const int index = FindTouchPoint(fingerId);
    if (index < 0) return;
    touchPoints[static_cast<size_t>(index)].position = glm::vec2(x, y);
}

void InputController::TrackTouchUp(SDL_FingerID fingerId) {
    const int index = FindTouchPoint(fingerId);
    if (index < 0) return;
    TouchPoint& point = touchPoints[static_cast<size_t>(index)];
    point.active = false;
    point.cameraEligible = false;
    point.fingerId = static_cast<SDL_FingerID>(-1);
    point.position = glm::vec2(0.0f);
}

bool InputController::UpdateTouchPinchState() {
    int first = -1;
    int second = -1;
    for (int i = 0; i < kTouchPointCount; ++i) {
        const TouchPoint& point = touchPoints[static_cast<size_t>(i)];
        if (!point.active || !point.cameraEligible) continue;
        if (first < 0) {
            first = i;
        } else {
            second = i;
            break;
        }
    }

    if (first >= 0 && second >= 0) {
        const float distance = glm::distance(
            touchPoints[static_cast<size_t>(first)].position,
            touchPoints[static_cast<size_t>(second)].position);
        if (!std::isfinite(distance)) return touchPinching;

        if (!touchPinching) {
            // 进入双指状态只建立基线，避免第二根手指落下时产生跳变。
            touchPinching = true;
            touchPinchLastDistance = distance;
            touchLookDeltaX = 0.0f;
            touchLookDeltaY = 0.0f;
            isTouchLooking = false;
        } else {
            // 限制单个事件的异常跳变；断帧或触摸采样丢失时不会瞬移相机。
            const float delta = glm::clamp(
                distance - touchPinchLastDistance, -100.0f, 100.0f);
            touchZoomDelta += delta;
            touchPinchLastDistance = distance;
        }
        return true;
    }

    if (touchPinching) {
        touchPinching = false;
        touchPinchLastDistance = 0.0f;
        touchLookDeltaX = 0.0f;
        touchLookDeltaY = 0.0f;

        // 捏合结束后，如果还有一根手指，继续用它转视角；从当前点建立基线，
        // 避免松开瞬间把手指位移误认为镜头拖动。
        if (first >= 0) {
            const TouchPoint& point = touchPoints[static_cast<size_t>(first)];
            isTouchLooking = true;
            lookFingerId = point.fingerId;
            lastTouchX = point.position.x;
            lastTouchY = point.position.y;
        } else {
            isTouchLooking = false;
        }
    }
    return false;
}

void InputController::ResetTouchTracking() {
    for (TouchPoint& point : touchPoints) {
        point.active = false;
        point.cameraEligible = false;
        point.fingerId = static_cast<SDL_FingerID>(-1);
        point.position = glm::vec2(0.0f);
    }
    touchPinching = false;
    touchPinchLastDistance = 0.0f;
    touchZoomDelta = 0.0f;
    touchTapPending = false;
    touchTapCandidate = false;
    touchTapFingerId = static_cast<SDL_FingerID>(-1);
    touchTapStartPosition = glm::vec2(0.0f);
    touchTapPosition = glm::vec2(0.0f);
}

glm::vec2 InputController::ConsumeTouchLookDelta() {
    const glm::vec2 delta(touchLookDeltaX, touchLookDeltaY);
    touchLookDeltaX = 0.0f;
    touchLookDeltaY = 0.0f;
    return delta;
}

float InputController::ConsumeTouchZoomDelta() {
    const float delta = touchZoomDelta;
    touchZoomDelta = 0.0f;
    return delta;
}

bool InputController::ConsumeTouchTap(glm::vec2& outPosition) {
    if (!touchTapPending) return false;
    outPosition = touchTapPosition;
    touchTapPending = false;
    touchTapPosition = glm::vec2(0.0f);
    return true;
}

bool InputController::IsTouchButtonDown(TouchAction action) const {
    const int index = TouchActionIndex(action);
    return touchEnabled && index >= 0 && touchButtonDown[static_cast<size_t>(index)];
}

bool InputController::ConsumeTouchButtonPressed(TouchAction action) {
    const int index = TouchActionIndex(action);
    if (!touchEnabled || index < 0) return false;

    const size_t slot = static_cast<size_t>(index);
    const bool pressed = touchButtonPressed[slot];
    touchButtonPressed[slot] = false;
    return pressed;
}

void InputController::SetJoystickConfig(const JoystickConfig& config) {
    moveJoystick.Init(config.moveBaseRadius, config.moveStickRadius, config.moveMaxDistance);
    lookJoystick.Init(config.lookBaseRadius, config.lookStickRadius, config.lookMaxDistance);
    mouseSensitivity = config.sensitivity;
    
    if (touchEnabled) {
        UpdateWindowSize();
        moveJoystick.SetPosition(config.moveOffsetX, windowHeight - config.moveOffsetY);
        lookJoystick.SetPosition(windowWidth - config.lookOffsetX, windowHeight - config.lookOffsetY);
        UpdateTouchButtonLayout(static_cast<float>(windowWidth), static_cast<float>(windowHeight));
    }
}

void InputController::SetMouseCapture(bool capture) {
    mouseCaptured = capture;
    firstMouse = true;

    SDL_Window* currentWindow = SDL_GetMouseFocus();
    if (!currentWindow) {
        currentWindow = window;
    }

    if (!currentWindow) {
        std::cout << "No window available for mouse capture" << std::endl;
        return;
    }

    window = currentWindow;

    if (capture) {
        //std::cout << "Enabling mouse capture" << std::endl;
        //std::cout << "Mouse sensitivity: " << mouseSensitivity << std::endl;

        if (SDL_SetWindowRelativeMouseMode(window, true)) {
            //std::cout << "Relative mouse mode enabled successfully" << std::endl;
        } else {
            //std::cout << "Failed to enable relative mouse mode: " << SDL_GetError() << std::endl;
            SDL_HideCursor();
        }

        SDL_CaptureMouse(true);
        CenterMouse();

    } else {
        //std::cout << "Disabling mouse capture" << std::endl;

        SDL_SetWindowRelativeMouseMode(window, false);
        SDL_CaptureMouse(false);
        SDL_ShowCursor();
    }
}

void InputController::ProcessMouse(Camera& camera, float deltaTime, SDL_Event& event) {
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        float normalizedX, normalizedY;
        NormalizeMousePosition(event.motion.x, event.motion.y, normalizedX, normalizedY);
        currentMouseX = normalizedX;
        currentMouseY = normalizedY;

        if (mouseCaptured) {
            if (firstMouse) {
                lastMouseX = windowWidth / 2;
                lastMouseY = windowHeight / 2;
                firstMouse = false;
                CenterMouse();
                return;
            }

            // SDL relative mouse mode already provides frame-independent deltas.
            // Using event.motion.x/y here is unreliable because the cursor is
            // recentered every event and can produce a zero/oscillating orbit delta.
            float xoffset = event.motion.xrel * mouseSensitivity;
            float yoffset = -event.motion.yrel * mouseSensitivity;

            CenterMouse();

            mouseDeltaX = xoffset;
            mouseDeltaY = yoffset;
            camera.ProcessMouseMovement(xoffset, yoffset);
        }
        else {
            // 普通窗口模式下优先使用 SDL 的相对增量；某些编辑器/窗口管理器
            // 只提供绝对坐标时，用相邻 motion 事件回退计算，保证右键拖拽仍可环绕。
            float xoffset = event.motion.xrel;
            float yoffset = event.motion.yrel;
            if (xoffset == 0.0f && yoffset == 0.0f && !firstMouse) {
                xoffset = event.motion.x - lastMouseX;
                yoffset = event.motion.y - lastMouseY;
            }
            lastMouseX = event.motion.x;
            lastMouseY = event.motion.y;
            firstMouse = false;
            mouseDeltaX += xoffset * mouseSensitivity;
            mouseDeltaY -= yoffset * mouseSensitivity;
        }
    }
}

void InputController::ProcessMouseButtons(SDL_Event& event) {
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.button == SDL_BUTTON_LEFT) {
            leftButtonPressed = true;
            leftButtonJustPressed = true;
        }
        else if (event.button.button == SDL_BUTTON_RIGHT) {
            rightButtonPressed = true;
            rightButtonJustPressed = true;
        }
    }
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event.button.button == SDL_BUTTON_LEFT) {
            leftButtonPressed = false;
        }
        else if (event.button.button == SDL_BUTTON_RIGHT) {
            rightButtonPressed = false;
        }
    }
}

bool InputController::IsMouseCaptured() const {
    return mouseCaptured;
}

glm::vec2 InputController::GetMousePosition() const {
    return glm::vec2(currentMouseX, currentMouseY);
}

glm::vec2 InputController::GetMouseDelta() const {
    return glm::vec2(mouseDeltaX, mouseDeltaY);
}

glm::vec2 InputController::ConsumeMouseDelta() {
    const glm::vec2 delta(mouseDeltaX, mouseDeltaY);
    mouseDeltaX = 0.0f;
    mouseDeltaY = 0.0f;
    return delta;
}

float InputController::ConsumeMouseWheel() {
    const float wheel = mouseWheelDelta;
    mouseWheelDelta = 0.0f;
    return wheel;
}

glm::vec4 InputController::GetMouseState() const {
    float clickX = (leftButtonDown || rightButtonDown) ? mouseClickPosition.x : 0.0f;
    float clickY = (leftButtonDown || rightButtonDown) ? mouseClickPosition.y : 0.0f;
    return glm::vec4(currentMouseX, currentMouseY, clickX, clickY);
}

bool InputController::IsMouseButtonDown(int button) const {
    if (button == SDL_BUTTON_LEFT) return leftButtonDown;
    if (button == SDL_BUTTON_RIGHT) return rightButtonDown;
    return false;
}

bool InputController::IsMouseButtonClicked(int button) const {
    if (button == SDL_BUTTON_LEFT) return (leftButtonPressed && !leftButtonWasPressed);
    if (button == SDL_BUTTON_RIGHT) return (rightButtonPressed && !rightButtonWasPressed);
    return false;
}

bool InputController::IsMouseButtonPressed(int button) const {
    if (button == SDL_BUTTON_LEFT) return leftButtonPressed;
    if (button == SDL_BUTTON_RIGHT) return rightButtonPressed;
    return false;
}

bool InputController::IsMouseButtonJustPressed(int button) const {
    if (button == SDL_BUTTON_LEFT) return (leftButtonPressed && !leftButtonWasPressed);
    if (button == SDL_BUTTON_RIGHT) return (rightButtonPressed && !rightButtonWasPressed);
    return false;
}

bool InputController::IsKeyDown(SDL_Scancode scancode) const {
    const bool* keyState = SDL_GetKeyboardState(nullptr);
    return keyState != nullptr && keyState[scancode];
}

void InputController::UpdateWindowSize() {
    SDL_Window* currentWindow = window;
    if (!currentWindow) currentWindow = SDL_GetMouseFocus();
    if (currentWindow) {
        window = currentWindow;
        int newWidth, newHeight;
        SDL_GetWindowSize(window, &newWidth, &newHeight);

        moveJoystick.SetScreenSize(static_cast<float>(newWidth), static_cast<float>(newHeight));
        
        if (newWidth != windowWidth || newHeight != windowHeight) {
            windowWidth = newWidth;
            windowHeight = newHeight;
            
            if (touchEnabled) {
                float offsetX = 150.0f;
                float offsetY = windowHeight - 150.0f;
                moveJoystick.SetPosition(offsetX, offsetY);
            }
        }
        UpdateTouchButtonLayout(static_cast<float>(windowWidth), static_cast<float>(windowHeight));
    }
}

int InputController::TouchActionIndex(TouchAction action) {
    const int index = static_cast<int>(action);
    return (index >= 0 && index < kTouchActionCount) ? index : -1;
}

void InputController::UpdateTouchButtonLayout(float width, float height) {
    const float safeWidth = std::max(1.0f, width);
    const float safeHeight = std::max(1.0f, height);

    // 以 2400x1080 横屏为基准，按高度缩放触控键；宽度只影响横向锚点。
    // 右侧必须为系统手势区/挖孔/圆角预留余量，不能再用固定 width-160。
    const float layoutScale = std::clamp(safeHeight / 1080.0f, 0.70f, 1.0f);
    const float ringPadding = 5.0f; // 绘制的外圈比命中圆半径多出的像素
    const float safeRightInset = TouchRightSafeInset(safeWidth, safeHeight);

    touchButtonRadii[0] = 84.0f * layoutScale;
    touchButtonRadii[1] = 74.0f * layoutScale;
    touchButtonRadii[2] = 74.0f * layoutScale;
    touchButtonRadii[3] = 66.0f * layoutScale;

    const float rightColumnX = safeWidth - safeRightInset -
                               touchButtonRadii[0] - ringPadding;
    const float leftColumnX = rightColumnX - 200.0f * layoutScale;
    touchButtonPositions[0] = glm::vec2(rightColumnX, safeHeight - 180.0f * layoutScale); // 攻击
    touchButtonPositions[1] = glm::vec2(leftColumnX, safeHeight - 120.0f * layoutScale);  // 跳跃
    touchButtonPositions[2] = glm::vec2(leftColumnX, safeHeight - 320.0f * layoutScale);  // 奔跑
    touchButtonPositions[3] = glm::vec2(rightColumnX, safeHeight - 380.0f * layoutScale); // 蹲伏

    // 最终按每个按键的实际外圈半径夹取，保证极窄/极矮窗口也不会被裁剪。
    for (int i = 0; i < kTouchActionCount; ++i) {
        const float radius = touchButtonRadii[static_cast<size_t>(i)] + ringPadding;
        touchButtonPositions[static_cast<size_t>(i)].x = std::clamp(
            touchButtonPositions[static_cast<size_t>(i)].x, radius, safeWidth - radius);
        touchButtonPositions[static_cast<size_t>(i)].y = std::clamp(
            touchButtonPositions[static_cast<size_t>(i)].y, radius, safeHeight - radius);
    }
}

int InputController::FindTouchButton(float x, float y) const {
    for (int i = 0; i < kTouchActionCount; ++i) {
        const glm::vec2 delta = glm::vec2(x, y) - touchButtonPositions[static_cast<size_t>(i)];
        const float hitRadius = touchButtonRadii[static_cast<size_t>(i)] * 1.15f;
        if (glm::dot(delta, delta) <= hitRadius * hitRadius) return i;
    }
    return -1;
}

void InputController::ResetTouchButtons() {
    touchButtonFingerIds.fill(static_cast<SDL_FingerID>(-1));
    touchButtonDown.fill(false);
    touchButtonPressed.fill(false);
    touchMouseButtonIndex = -1;
}

bool InputController::HandleTouchButtonDown(float x, float y, SDL_FingerID fingerId) {
    const int index = FindTouchButton(x, y);
    if (index < 0) return false;

    const size_t slot = static_cast<size_t>(index);
    if (!touchButtonDown[slot]) {
        touchButtonDown[slot] = true;
        touchButtonPressed[slot] = true;
        touchButtonFingerIds[slot] = fingerId;
    }
    return true;
}

bool InputController::HandleTouchButtonMotion(float x, float y, SDL_FingerID fingerId) {
    for (int i = 0; i < kTouchActionCount; ++i) {
        const size_t slot = static_cast<size_t>(i);
        if (touchButtonFingerIds[slot] != fingerId) continue;

        touchButtonDown[slot] = FindTouchButton(x, y) == i;
        return true;
    }
    return false;
}

bool InputController::HandleTouchButtonUp(SDL_FingerID fingerId) {
    for (int i = 0; i < kTouchActionCount; ++i) {
        const size_t slot = static_cast<size_t>(i);
        if (touchButtonFingerIds[slot] != fingerId) continue;

        touchButtonDown[slot] = false;
        touchButtonFingerIds[slot] = static_cast<SDL_FingerID>(-1);
        return true;
    }
    return false;
}

void InputController::NormalizeMousePosition(float x, float y, float& outX, float& outY) {
    if (windowWidth > 0 && windowHeight > 0) {
        outX = x / windowWidth;
        outY = y / windowHeight;
        outX = glm::clamp(outX, 0.0f, 1.0f);
        outY = glm::clamp(outY, 0.0f, 1.0f);
    }
    else {
        outX = outY = 0.0f;
    }
}

// 获取键盘输入的移动偏移
glm::vec3 InputController::GetKeyboardMovementOffset(float deltaTime) const {
    const bool* keyState = SDL_GetKeyboardState(NULL);
    
    glm::vec3 moveVector(0.0f);
    
    if (keyState[SDL_SCANCODE_W]) moveVector.z -= 1.0f; // 向前
    if (keyState[SDL_SCANCODE_S]) moveVector.z += 1.0f; // 向后
    if (keyState[SDL_SCANCODE_A]) moveVector.x -= 1.0f; // 向左
    if (keyState[SDL_SCANCODE_D]) moveVector.x += 1.0f; // 向右
    if (keyState[SDL_SCANCODE_SPACE]) moveVector.y += 1.0f; // 向上
    if (keyState[SDL_SCANCODE_LSHIFT] || keyState[SDL_SCANCODE_RSHIFT]) moveVector.y -= 1.0f; // 向下
    
    // 标准化移动向量
    if (glm::length(moveVector) > 0.0f) {
        moveVector = glm::normalize(moveVector);
    }
    
    // 应用移动速度和时间增量
    const float moveSpeed = 5.0f;
    moveVector *= moveSpeed * deltaTime;
    
    return moveVector;
}

void InputController::FindSceneCamera() {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    
    const auto& rootEntities = sceneECS.GetRootEntities();
    for (ECS::Entity entity : rootEntities) {
        if (coordinator.HasComponent<ECS::CameraComponent>(entity) && 
            coordinator.GetComponent<ECS::CameraComponent>(entity).isMainCamera) {
            sceneCameraEntity = entity;
            hasSceneCamera = true;
            
            if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
                auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
                glm::vec3 forward = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
                
                sceneCameraPitch = glm::degrees(asin(forward.y));
                sceneCameraYaw = glm::degrees(atan2(forward.z, forward.x));
                
                if (sceneCameraYaw > 180.0f) sceneCameraYaw -= 360.0f;
                if (sceneCameraYaw < -180.0f) sceneCameraYaw += 360.0f;
                if (sceneCameraPitch > 89.0f) sceneCameraPitch = 89.0f;
                if (sceneCameraPitch < -89.0f) sceneCameraPitch = -89.0f;
            }
            
            return;
        }
    }
    
    hasSceneCamera = false;
    sceneCameraEntity = ECS::INVALID_ENTITY;
}

void InputController::UpdateSceneCamera(float deltaTime) {
    // Gameplay script owns the scene camera (e.g. third-person follow) - engine yields control
    if (IsSceneCameraControlLocked()) {
        return;
    }

    if (!hasSceneCamera || sceneCameraEntity == ECS::INVALID_ENTITY) {
        FindSceneCamera();
        return;
    }
    
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::TransformComponent>(sceneCameraEntity) ||
        !coordinator.HasComponent<ECS::CameraComponent>(sceneCameraEntity)) {
        return;
    }
    
    auto& transform = coordinator.GetComponent<ECS::TransformComponent>(sceneCameraEntity);
    auto& camera = coordinator.GetComponent<ECS::CameraComponent>(sceneCameraEntity);

    // 第三人称系统接管主相机实体，避免这里的自由相机输入与轨道相机互相覆盖。
    if (camera.thirdPersonEnabled) {
        return;
    }
    
    ProcessKeyboardForSceneCamera(deltaTime, camera, transform);
    ProcessMouseForSceneCamera(deltaTime, camera, transform);
    ProcessTouchForSceneCamera(deltaTime, camera, transform);
}

void InputController::ProcessKeyboardForSceneCamera(float deltaTime, ECS::CameraComponent& camera, ECS::TransformComponent& transform) {
    const bool* keyState = SDL_GetKeyboardState(NULL);
    
    glm::vec3 front = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 right = transform.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    
    float velocity = 20.0f * deltaTime;
    glm::vec3 moveDirection(0.0f);
    
    if (keyState[SDL_SCANCODE_W]) {
        moveDirection += front;
    }
    if (keyState[SDL_SCANCODE_S]) {
        moveDirection -= front;
    }
    if (keyState[SDL_SCANCODE_A]) {
        moveDirection -= right;
    }
    if (keyState[SDL_SCANCODE_D]) {
        moveDirection += right;
    }
    if (keyState[SDL_SCANCODE_SPACE]) {
        moveDirection += up;
    }
    if (keyState[SDL_SCANCODE_LSHIFT] || keyState[SDL_SCANCODE_RSHIFT]) {
        moveDirection -= up;
    }
    
    if (glm::length(moveDirection) > 0.0f) {
        moveDirection = glm::normalize(moveDirection);
    }
    
    transform.position += moveDirection * velocity;
}

void InputController::ProcessMouseForSceneCamera(float deltaTime, ECS::CameraComponent& camera, ECS::TransformComponent& transform) {
    if (mouseCaptured) {
        if (firstMouse) {
            lastMouseX = windowWidth / 2;
            lastMouseY = windowHeight / 2;
            firstMouse = false;
            CenterMouse();
            return;
        }
        
        float xoffset = mouseDeltaX;
        float yoffset = mouseDeltaY;
        
        if (xoffset != 0.0f || yoffset != 0.0f) {
            xoffset *= 0.1f;
            yoffset *= 0.1f;
            
            sceneCameraYaw += xoffset;
            sceneCameraPitch += yoffset;
            
            if (sceneCameraPitch > 89.0f) sceneCameraPitch = 89.0f;
            if (sceneCameraPitch < -89.0f) sceneCameraPitch = -89.0f;
            
            if (sceneCameraYaw > 180.0f) sceneCameraYaw -= 360.0f;
            if (sceneCameraYaw < -180.0f) sceneCameraYaw += 360.0f;
            
            glm::vec3 front;
            front.x = cos(glm::radians(sceneCameraYaw)) * cos(glm::radians(sceneCameraPitch));
            front.y = sin(glm::radians(sceneCameraPitch));
            front.z = sin(glm::radians(sceneCameraYaw)) * cos(glm::radians(sceneCameraPitch));
            front = glm::normalize(front);
            
            glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
            glm::vec3 up = glm::normalize(glm::cross(right, front));
            
            transform.rotation = glm::quatLookAt(front, up);
            transform.MarkDirty(); // 场景相机旋转,世界矩阵缓存需失效
            
            mouseDeltaX = 0.0f;
            mouseDeltaY = 0.0f;
        }
    } else {
        if (firstMouse) {
            lastMouseX = windowWidth / 2;
            lastMouseY = windowHeight / 2;
            firstMouse = false;
        }
    }
}

void InputController::ProcessTouchForSceneCamera(float deltaTime, ECS::CameraComponent& camera, ECS::TransformComponent& transform) {
    if (!touchEnabled) return;
    
    glm::vec2 moveDir = moveJoystick.GetDirection();
    
    if (glm::length(moveDir) > 0.1f) {
        glm::vec3 front = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 right = transform.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        
        float velocity = 20.0f * deltaTime;
        glm::vec3 moveDirection(0.0f);
        
        if (moveDir.y < -0.3f) {
            moveDirection += front;
        }
        if (moveDir.y > 0.3f) {
            moveDirection -= front;
        }
        if (moveDir.x < -0.3f) {
            moveDirection -= right;
        }
        if (moveDir.x > 0.3f) {
            moveDirection += right;
        }
        
        if (glm::length(moveDirection) > 0.0f) {
            moveDirection = glm::normalize(moveDirection);
            transform.position += moveDirection * velocity;
            transform.MarkDirty(); // 虚拟摇杆位移,世界矩阵缓存需失效
        }
    }
    
    if (isTouchLooking) {
        // 应用累加的增量值
        float xoffset = touchLookDeltaX * 0.1f;
        float yoffset = -touchLookDeltaY * 0.1f;
        
        sceneCameraYaw += xoffset;
        sceneCameraPitch += yoffset;
        
        if (sceneCameraPitch > 89.0f) sceneCameraPitch = 89.0f;
        if (sceneCameraPitch < -89.0f) sceneCameraPitch = -89.0f;
        
        if (sceneCameraYaw > 180.0f) sceneCameraYaw -= 360.0f;
        if (sceneCameraYaw < -180.0f) sceneCameraYaw += 360.0f;
        
        glm::vec3 front;
        front.x = cos(glm::radians(sceneCameraYaw)) * cos(glm::radians(sceneCameraPitch));
        front.y = sin(glm::radians(sceneCameraPitch));
        front.z = sin(glm::radians(sceneCameraYaw)) * cos(glm::radians(sceneCameraPitch));
        front = glm::normalize(front);
        
        glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
        glm::vec3 up = glm::normalize(glm::cross(right, front));
        
        transform.rotation = glm::quatLookAt(front, up);
        transform.MarkDirty(); // 触摸视角旋转,世界矩阵缓存需失效
        
        // 每帧应用后清零
        touchLookDeltaX = 0.0f;
        touchLookDeltaY = 0.0f;
    }
}

void InputController::ProcessModeToggle() {
    const bool* keyState = SDL_GetKeyboardState(NULL);
    
    if (keyState[SDL_SCANCODE_F1]) {
        extern RunMode g_RunMode;
        if (g_RunMode == RunMode::Editor) {
            g_RunMode = RunMode::Game;
            std::cout << "切换到游戏模式" << std::endl;
            
            FindSceneCamera();
            if (hasSceneCamera) {
                SetMouseCapture(true);
                std::cout << "已锁定场景相机实体：" << sceneCameraEntity << std::endl;
            } else {
                std::cout << "未找到场景主相机实体" << std::endl;
            }
        } else {
            g_RunMode = RunMode::Editor;
            std::cout << "切换到编辑器模式" << std::endl;
            
            SetMouseCapture(false);
            sceneCameraEntity = ECS::INVALID_ENTITY;
            hasSceneCamera = false;
        }
        
        SDL_Delay(200);
    }
}
