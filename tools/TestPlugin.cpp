// TestPlugin.cpp - 最小游戏插件示例(验证独立 DLL 加载,引擎本体零修改)
// 编译为 DLL 放入 exe 目录,引擎 --game testplug 时自动 LoadLibrary 加载。
// 约定导出: GetGameModuleName / CreateGameModule
//
// 编译命令(VsDevCmd x64 环境;注意 /utf-8 + MIKAN_USE_GAME + 链接 Game.lib):
//   cl /nologo /LD /EHsc /std:c++17 /utf-8 /DMIKAN_USE_GAME ^
//       /I "D:\Engine project\vulkan engine\include" /I "D:\Engine project\vulkan engine\dependencies" ^
//       TestPlugin.cpp /Fe:testplug.dll ^
//       /link "D:\Engine project\vulkan engine\out\build\x64-Release\Game.lib"
#include "Game/IGameModule.h"
#include "Core/RenderGlobals.h"
#include "Rendering/Renderer2D.h"
#include <SDL3/SDL.h>

// 演示: 一个左右移动的色块 + FPS(证明插件游戏有画面/逻辑)
class TestPlugGame : public Game::IGameModule {
public:
    static TestPlugGame& GetInstance() { static TestPlugGame i; return i; }

    const char* GetName() const override { return "testplug"; }
    void OnSceneLoaded() override {}
    void OnUpdate(float dt) override {
        m_x += dt * 300.0f;
        if (m_x > 1800.0f) m_x = 0.0f;
    }
    void OnKey(SDL_Keycode) override {}
    void OnRenderUI(Renderer2D& r2d, int w, int h) override {
        // 移动色块(UI 层屏幕坐标,画布 y 向下)
        r2d.DrawRect(glm::vec2(m_x, h * 0.5f), glm::vec2(120.0f, 120.0f),
                     glm::vec4(0.3f, 0.8f, 0.4f, 1.0f), 0);
        // FPS(插件游戏自身绘制,演示全局 g_ShowFPS/g_FPS)
        char buf[64];
        snprintf(buf, sizeof(buf), "plugin FPS: %.0f", g_FPS);
        r2d.DrawRect(glm::vec2(20.0f, h - 80.0f), glm::vec2(220.0f, 40.0f),
                     glm::vec4(0.1f, 0.1f, 0.12f, 0.8f), 1);
    }

private:
    float m_x = 0.0f;
};

extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "testplug";
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &TestPlugGame::GetInstance();
}
