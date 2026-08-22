#include "InputController.h"
#include "imgui.h"
#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "EngineGlobal.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"

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
    sceneCameraEntity(ECS::INVALID_ENTITY), hasSceneCamera(false),
    sceneCameraYaw(-90.0f), sceneCameraPitch(0.0f),
    sceneCameraTouchLooking(false), lastSceneTouchX(0), lastSceneTouchY(0)
{
    moveJoystick.Init(80.0f, 50.0f, 60.0f);
    lookJoystick.Init(80.0f, 50.0f, 60.0f);
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
            float x = event.tfinger.x * ImGui::GetIO().DisplaySize.x;
            float y = event.tfinger.y * ImGui::GetIO().DisplaySize.y;
            
            if (event.type == SDL_EVENT_FINGER_DOWN) {
                if (moveJoystick.IsPointInCircle(x, y, moveJoystick.GetPosition().x, moveJoystick.GetPosition().y, moveJoystick.GetBaseRadius() * 1.5f)) {
                    moveJoystick.HandleTouch(event);
                } else {
                    lastTouchX = x;
                    lastTouchY = y;
                    isTouchLooking = true;
                    lookFingerId = event.tfinger.fingerID;
                    touchLookDeltaX = 0.0f;
                    touchLookDeltaY = 0.0f;
                }
            } else if (event.type == SDL_EVENT_FINGER_MOTION) {
                if (isTouchLooking && event.tfinger.fingerID == lookFingerId) {
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
                } else {
                    moveJoystick.HandleTouch(event);
                }
            } else if (event.type == SDL_EVENT_FINGER_UP) {
                if (isTouchLooking && event.tfinger.fingerID == lookFingerId) {
                    isTouchLooking = false;
                    touchLookDeltaX = 0.0f;
                    touchLookDeltaY = 0.0f;
                } else {
                    moveJoystick.HandleTouch(event);
                }
            }
        }
    }

    if (touchEnabled && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
        float x = (float)event.button.x;
        float y = (float)event.button.y;
        
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (moveJoystick.IsPointInCircle(x, y, moveJoystick.GetPosition().x, moveJoystick.GetPosition().y, moveJoystick.GetBaseRadius() * 1.5f)) {
                moveJoystick.HandleTouch(event);
            } else {
                lastTouchX = x;
                lastTouchY = y;
                isTouchLooking = true;
                touchLookDeltaX = 0.0f;
                touchLookDeltaY = 0.0f;
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            if (isTouchLooking) {
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
            } else {
                moveJoystick.HandleTouch(event);
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (isTouchLooking) {
                isTouchLooking = false;
                touchLookDeltaX = 0.0f;
                touchLookDeltaY = 0.0f;
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

void InputController::SetTouchEnabled(bool enabled) {
    touchEnabled = enabled;
    moveJoystick.SetEnabled(enabled);
    lookJoystick.SetEnabled(false);

    if (enabled) {
        UpdateWindowSize();
        float offsetX = 150.0f;
        float offsetY = windowHeight - 150.0f;
        moveJoystick.SetPosition(offsetX, offsetY);
    }
}

bool InputController::IsTouchEnabled() const {
    return touchEnabled;
}

void InputController::SetJoystickConfig(const JoystickConfig& config) {
    moveJoystick.Init(config.moveBaseRadius, config.moveStickRadius, config.moveMaxDistance);
    lookJoystick.Init(config.lookBaseRadius, config.lookStickRadius, config.lookMaxDistance);
    mouseSensitivity = config.sensitivity;
    
    if (touchEnabled) {
        UpdateWindowSize();
        moveJoystick.SetPosition(config.moveOffsetX, windowHeight - config.moveOffsetY);
        lookJoystick.SetPosition(windowWidth - config.lookOffsetX, windowHeight - config.lookOffsetY);
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
    SDL_Window* currentWindow = SDL_GetMouseFocus();
    if (currentWindow) {
        window = currentWindow;
        int newWidth, newHeight;
        SDL_GetWindowSize(window, &newWidth, &newHeight);
        
        if (newWidth != windowWidth || newHeight != windowHeight) {
            windowWidth = newWidth;
            windowHeight = newHeight;
            
            if (touchEnabled) {
                float offsetX = 150.0f;
                float offsetY = windowHeight - 150.0f;
                moveJoystick.SetPosition(offsetX, offsetY);
            }
        }
    }
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
