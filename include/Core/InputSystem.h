#pragma once
// InputSystem.h - 引擎标准输入接口(动作映射 Action Map)
// 游戏通过语义化动作名查询输入, 不直接读 SDL 键码:
//   Input::InputSystem::GetInstance().IsPressed("Confirm");
// 动作可绑定多个按键/鼠标按钮(主键+备键), 支持运行时重绑定。
// 状态语义: Pressed=本帧刚按下(边沿), Down=按住(电平), Released=本帧释放。
// 坐标: 鼠标位置为画布坐标(左下原点, y 向下, 与 Canvas2D 一致)。
#include "Platform/Export.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>

namespace Input {

class MIKAN_API InputSystem {
public:
    static InputSystem& GetInstance();

    // 每帧调用(主循环, 事件处理后): 快照键盘/鼠标状态并计算 pressed/released
    void Update();

    // ===== 动作查询(布尔)=====
    bool IsDown(const char* action) const;     // 按住
    bool IsPressed(const char* action) const;  // 本帧刚按下(边沿, 只触发一次)
    bool IsReleased(const char* action) const; // 本帧释放
    // 任意键/鼠标刚按下(菜单"按任意键启动"等)
    bool AnyKeyPressed() const;

    // 轴查询: negAction 按住 → -1, posAction 按住 → +1, 都无 → 0(如 MoveLeft/MoveRight)
    float GetAxis(const char* negAction, const char* posAction) const;

    // ===== 鼠标(画布坐标, y 向下)=====
    glm::vec2 GetMousePosition() const;
    bool IsMouseDown(int button) const;     // button: 1=左 2=右 3=中
    bool IsMousePressed(int button) const;
    bool IsMouseReleased(int button) const;

    // ===== 绑定 =====
    void BindKey(const char* action, SDL_Keycode key);   // 追加一个按键
    void BindMouseButton(const char* action, int button); // 追加一个鼠标按钮
    void UnbindAction(const char* action);               // 清空动作的全部绑定
    bool HasAction(const char* action) const;
    // 动作当前绑定的键(调试/重绑定 UI 用)
    const std::vector<SDL_Keycode>& GetKeys(const char* action) const;

    // 引擎内置默认动作集(游戏可覆盖):
    //   Confirm(Enter/空格) Cancel(Esc) Restart(R) Pause(P)
    //   MoveLeft(A/←) MoveRight(D/→) MoveUp(W/↑) MoveDown(S/↓)
    void LoadDefaultBindings();

private:
    InputSystem();
    ~InputSystem() = default;
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    bool KeyDown(SDL_Keycode key) const;     // 该键当前是否按住(scancode)
    bool KeyPressed(SDL_Keycode key) const;
    bool KeyReleased(SDL_Keycode key) const;

    struct ActionBinding {
        std::vector<SDL_Keycode> keys;
        std::vector<int> mouseButtons; // 1..5
    };

    std::unordered_map<std::string, ActionBinding> m_actions;
    std::vector<SDL_Keycode> m_emptyKeys;

    // 键盘 scancode 状态(按 scancode 索引)
    static constexpr int kMaxScancodes = 512;
    bool m_keyDown[kMaxScancodes] = {};
    bool m_keyPrev[kMaxScancodes] = {};
    // 鼠标按钮状态(1..5)
    bool m_mouseDown[6] = {};
    bool m_mousePrev[6] = {};
};

} // namespace Input
