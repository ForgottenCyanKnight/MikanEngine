// InputSystem.cpp - 引擎标准输入接口(动作映射)
#include "Core/InputSystem.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace Input {

InputSystem& InputSystem::GetInstance() {
    static InputSystem instance;
    return instance;
}

InputSystem::InputSystem() {
    LoadDefaultBindings();
}

void InputSystem::LoadDefaultBindings() {
    m_actions.clear();
    BindKey("Confirm", SDLK_RETURN);
    BindKey("Confirm", SDLK_SPACE);
    BindKey("Cancel", SDLK_ESCAPE);
    BindKey("Restart", SDLK_R);
    BindKey("Pause", SDLK_P);
    BindKey("Jump", SDLK_SPACE); // 跳跃 = 空格
    BindKey("MoveLeft", SDLK_A);
    BindKey("MoveLeft", SDLK_LEFT);
    BindKey("MoveRight", SDLK_D);
    BindKey("MoveRight", SDLK_RIGHT);
    BindKey("MoveUp", SDLK_W);
    BindKey("MoveUp", SDLK_UP);
    BindKey("MoveDown", SDLK_S);
    BindKey("MoveDown", SDLK_DOWN);
}

void InputSystem::SetSyntheticState(const glm::vec2& move, bool jump) {
    m_syntheticJumpPrevious = m_syntheticJump;
    m_syntheticMove = glm::clamp(move, glm::vec2(-1.0f), glm::vec2(1.0f));
    m_syntheticJump = jump;
    m_syntheticEnabled = true;
}

void InputSystem::ClearSyntheticState() {
    m_syntheticEnabled = false;
    m_syntheticMove = glm::vec2(0.0f);
    m_syntheticJump = false;
    m_syntheticJumpPrevious = false;
}

void InputSystem::Update() {
    // 键盘: 快照 scancode 状态
    std::memcpy(m_keyPrev, m_keyDown, sizeof(m_keyDown));
    const bool* ks = SDL_GetKeyboardState(nullptr); // SDL3: 返回 const bool*
    if (ks) {
        for (int sc = 0; sc < kMaxScancodes; ++sc) {
            m_keyDown[sc] = ks[sc];
        }
    } else {
        std::memset(m_keyDown, 0, sizeof(m_keyDown));
    }

    // 鼠标: 快照按钮状态(1..5)
    std::memcpy(m_mousePrev, m_mouseDown, sizeof(m_mouseDown));
    float mx = 0.0f, my = 0.0f;
    Uint32 mb = SDL_GetMouseState(&mx, &my);
    for (int b = 1; b <= 5; ++b) {
        m_mouseDown[b] = (mb & SDL_BUTTON_MASK(b)) != 0;
    }
}

int ScancodeOf(SDL_Keycode key) {
    return (int)SDL_GetScancodeFromKey(key, nullptr); // SDL3: 第二参数 modstate 可传 nullptr
}

bool InputSystem::KeyDown(SDL_Keycode key) const {
    int sc = ScancodeOf(key);
    if (sc < 0 || sc >= kMaxScancodes) return false;
    return m_keyDown[sc];
}

bool InputSystem::KeyPressed(SDL_Keycode key) const {
    int sc = ScancodeOf(key);
    if (sc < 0 || sc >= kMaxScancodes) return false;
    return m_keyDown[sc] && !m_keyPrev[sc];
}

bool InputSystem::KeyReleased(SDL_Keycode key) const {
    int sc = ScancodeOf(key);
    if (sc < 0 || sc >= kMaxScancodes) return false;
    return !m_keyDown[sc] && m_keyPrev[sc];
}

bool InputSystem::IsSyntheticActionDown(const char* action) const {
    if (!m_syntheticEnabled || !action) return false;
    if (std::strcmp(action, "MoveLeft") == 0) return m_syntheticMove.x < -0.001f;
    if (std::strcmp(action, "MoveRight") == 0) return m_syntheticMove.x > 0.001f;
    if (std::strcmp(action, "MoveUp") == 0) return m_syntheticMove.y > 0.001f;
    if (std::strcmp(action, "MoveDown") == 0) return m_syntheticMove.y < -0.001f;
    if (std::strcmp(action, "Jump") == 0) return m_syntheticJump;
    return false;
}

bool InputSystem::IsDown(const char* action) const {
    if (m_syntheticEnabled) return IsSyntheticActionDown(action);
    auto it = m_actions.find(action ? action : "");
    if (it == m_actions.end()) return false;
    for (SDL_Keycode k : it->second.keys) { if (KeyDown(k)) return true; }
    for (int b : it->second.mouseButtons) { if (b >= 1 && b <= 5 && m_mouseDown[b]) return true; }
    return false;
}

bool InputSystem::IsPressed(const char* action) const {
    if (m_syntheticEnabled) {
        return action && std::strcmp(action, "Jump") == 0 &&
               m_syntheticJump && !m_syntheticJumpPrevious;
    }
    auto it = m_actions.find(action ? action : "");
    if (it == m_actions.end()) return false;
    for (SDL_Keycode k : it->second.keys) { if (KeyPressed(k)) return true; }
    for (int b : it->second.mouseButtons) { if (b >= 1 && b <= 5 && m_mouseDown[b] && !m_mousePrev[b]) return true; }
    return false;
}

bool InputSystem::IsReleased(const char* action) const {
    if (m_syntheticEnabled) {
        return action && std::strcmp(action, "Jump") == 0 &&
               !m_syntheticJump && m_syntheticJumpPrevious;
    }
    auto it = m_actions.find(action ? action : "");
    if (it == m_actions.end()) return false;
    for (SDL_Keycode k : it->second.keys) { if (KeyReleased(k)) return true; }
    for (int b : it->second.mouseButtons) { if (b >= 1 && b <= 5 && !m_mouseDown[b] && m_mousePrev[b]) return true; }
    return false;
}

bool InputSystem::AnyKeyPressed() const {
    for (int sc = 0; sc < kMaxScancodes; ++sc) {
        if (m_keyDown[sc] && !m_keyPrev[sc]) return true;
    }
    for (int b = 1; b <= 5; ++b) {
        if (m_mouseDown[b] && !m_mousePrev[b]) return true;
    }
    return false;
}

float InputSystem::GetAxis(const char* negAction, const char* posAction) const {
    float axis = 0.0f;
    if (IsDown(negAction)) axis -= 1.0f;
    if (IsDown(posAction)) axis += 1.0f;
    return axis;
}

glm::vec2 InputSystem::GetMousePosition() const {
    float mx = 0.0f, my = 0.0f;
    SDL_GetMouseState(&mx, &my);
    return glm::vec2(mx, my); // 屏幕像素坐标(y 向下, 与画布一致)
}

bool InputSystem::IsMouseDown(int button) const {
    return button >= 1 && button <= 5 && m_mouseDown[button];
}
bool InputSystem::IsMousePressed(int button) const {
    return button >= 1 && button <= 5 && m_mouseDown[button] && !m_mousePrev[button];
}
bool InputSystem::IsMouseReleased(int button) const {
    return button >= 1 && button <= 5 && !m_mouseDown[button] && m_mousePrev[button];
}

void InputSystem::BindKey(const char* action, SDL_Keycode key) {
    if (!action) return;
    m_actions[action].keys.push_back(key);
}

void InputSystem::BindMouseButton(const char* action, int button) {
    if (!action || button < 1 || button > 5) return;
    m_actions[action].mouseButtons.push_back(button);
}

void InputSystem::UnbindAction(const char* action) {
    if (action) m_actions.erase(action);
}

bool InputSystem::HasAction(const char* action) const {
    return action && m_actions.count(action) > 0;
}

const std::vector<SDL_Keycode>& InputSystem::GetKeys(const char* action) const {
    auto it = m_actions.find(action ? action : "");
    return (it != m_actions.end()) ? it->second.keys : m_emptyKeys;
}

} // namespace Input
