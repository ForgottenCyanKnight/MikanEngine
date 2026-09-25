
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "Core/Utf8Path.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include "Core/Log.h"
#include "Core/EngineAssets.h"
#include "Core/GameplayRuntime.h"
#include "Core/EngineCommandLine.h"
#include "Core/EngineDiagnostics.h"
#include "Core/EngineShutdown.h"
#include "Core/RenderDocCapture.h"
#include "Core/ScreenshotCapture.h"
#include <fstream>
#include "Camera.h"
#include "InputController.h"
#include "ECS/SceneECS.h"
#include "ECS/ScriptSystem.h"
#include <cstring>
#include "SceneSerializer.h"
#include "Core/ProjectManager.h"
#include "Core/AutosaveService.h"
#include <filesystem>
#include <locale>
#include <codecvt>
#include "Rendering/SceneRenderer.h"
#include "Rendering/InfiniteGridRenderer.h"
#include "Rendering/ParticleSystem.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/FontAtlas.h"
#include "Rendering/TextRenderer.h"
#include "UI/Canvas2D.h"
#include "UI/TweenSystem.h"
#include "ECS/Systems/SpriteAnimatorSystem.h"
#include "ECS/Systems/AudioSourceSystem.h"
#include "ECS/Systems/VmdSystem.h"
#include "Game/GameManager.h"
#include "Core/InputSystem.h"
#include "Core/Physics2DManager.h"
#include "Core/Physics2DSystem.h"
#include "Core/Camera2DSystem.h"
#include "Core/ThirdPersonCameraSystem.h"
#include "Core/PlayerControllerSystem.h"
#include "Core/TilemapSystem.h"
#include "UI/RuntimeSettingsOverlay.h"
#include "box2d/box2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <functional>
#include <SDL3/SDL.h>
// Windows: main() 在 HostMain.exe，此处用 MikanEngineMain 供宿主调用 → 不重命名。
// Android: SDLActivity 从 .so 查找导出的 SDL_main 符号，必须让 SDL_main.h 重命名 main → SDL_main。
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <array>

// 声明全局清理函数
extern void CleanupPhysicsSystem();
#include <fstream>
#include <algorithm>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3_image/SDL_image.h>
#include <chrono>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>

// Android 平台使用 logcat 输出日志
#ifdef __ANDROID__
#include <android/log.h>
#include <fstream>
#include <jni.h>
#include <SDL3/SDL_system.h>
#endif

// Windows 下设置 UTF-8 编码支持中文输出
#ifdef _WIN32
#include <windows.h>
#include <gdiplus.h>
#include <psapi.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "psapi.lib")
#endif

#ifdef _WIN32
// Windows 原生启动覆盖层：在 Vulkan 尚未提交第一帧前显示与 Android
// Activity 相同的 Logo，避免 SDL 窗口创建后长时间露出黑屏。覆盖层使用
// 与引擎相同尺寸的普通窗口，不切换系统全屏。
class DesktopStartupSplashOverlay {
public:
    ~DesktopStartupSplashOverlay() { Hide(); }

    bool Show(const std::string& imagePath,
              int clientWidth, int clientHeight,
              int windowX, int windowY) {
        if (m_window != nullptr) return true;
        if (clientWidth <= 0 || clientHeight <= 0) return false;

        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &startupInput, nullptr) !=
            Gdiplus::Ok) {
            m_gdiplusToken = 0;
            return false;
        }

        const std::wstring widePath = mikanpath::Utf8ToWide(imagePath);
        m_image = Gdiplus::Image::FromFile(widePath.c_str(), FALSE);
        if (m_image == nullptr || m_image->GetLastStatus() != Gdiplus::Ok) {
            Hide();
            return false;
        }

        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = &WindowProc;
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (RegisterClassExW(&windowClass) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Hide();
            return false;
        }

        constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW;
        constexpr DWORD extendedStyle =
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        RECT windowRect{0, 0, clientWidth, clientHeight};
        if (!AdjustWindowRectEx(&windowRect, windowStyle, FALSE, extendedStyle)) {
            Hide();
            return false;
        }

        const int outerWidth = windowRect.right - windowRect.left;
        const int outerHeight = windowRect.bottom - windowRect.top;
        m_window = CreateWindowExW(
            extendedStyle, kWindowClassName, L"Mikan Engine - Vulkan",
            windowStyle, windowX, windowY, outerWidth, outerHeight,
            nullptr, nullptr, instance, this);
        if (m_window == nullptr) {
            Hide();
            return false;
        }

        SetWindowPos(m_window, HWND_TOPMOST,
                     windowX, windowY, outerWidth, outerHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(m_window, SW_SHOWNOACTIVATE);
        UpdateWindow(m_window);
        return true;
    }

    void Hide() {
        if (m_window != nullptr) {
            DestroyWindow(m_window);
            m_window = nullptr;
        }
        delete m_image;
        m_image = nullptr;
        if (m_gdiplusToken != 0) {
            Gdiplus::GdiplusShutdown(m_gdiplusToken);
            m_gdiplusToken = 0;
        }
    }

private:
    static constexpr wchar_t kWindowClassName[] = L"MikanEngineDesktopStartupSplash";

    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<DesktopStartupSplashOverlay*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<DesktopStartupSplashOverlay*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr) {
            if (message == WM_ERASEBKGND) return 1;
            if (message == WM_PAINT) return self->Paint(window);
            if (message == WM_NCHITTEST) return HTTRANSPARENT;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT Paint(HWND window) {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);

        Gdiplus::Graphics graphics(deviceContext);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.Clear(Gdiplus::Color(252, 251, 240));

        if (m_image != nullptr) {
            const float imageWidth = static_cast<float>(m_image->GetWidth());
            const float imageHeight = static_cast<float>(m_image->GetHeight());
            const float clientWidth = static_cast<float>(client.right - client.left);
            const float clientHeight = static_cast<float>(client.bottom - client.top);
            const float scale = std::max(clientWidth / imageWidth,
                                         clientHeight / imageHeight);
            const float drawWidth = imageWidth * scale;
            const float drawHeight = imageHeight * scale;
            const Gdiplus::RectF target(
                (clientWidth - drawWidth) * 0.5f,
                (clientHeight - drawHeight) * 0.5f,
                drawWidth, drawHeight);
            graphics.DrawImage(m_image, target, 0.0f, 0.0f,
                               imageWidth, imageHeight, Gdiplus::UnitPixel);
        }

        EndPaint(window, &paint);
        return 0;
    }

    ULONG_PTR m_gdiplusToken = 0;
    Gdiplus::Image* m_image = nullptr;
    HWND m_window = nullptr;
};

constexpr wchar_t DesktopStartupSplashOverlay::kWindowClassName[];
#endif

// ==================== 崩溃诊断（SEH） ====================
// 捕获未处理异常，把异常码/地址/调用栈写入 crash_log.txt，便于定位崩溃
#ifdef _WIN32
#include <cstdio>
static char s_crashDumpPath[4096] = "log/crash_log.txt";

static void ConfigureCrashDumpPath(int argc, char* argv[]) {
    std::string path = "log/crash_log.txt";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg.rfind("--crash-log=", 0) == 0) {
            path = arg.substr(12);
        } else if (arg == "--crash-log" && i + 1 < argc) {
            path = argv[++i] ? argv[i] : "";
        }
    }
    if (path.empty()) path = "log/crash_log.txt";
    std::error_code ec;
    const std::filesystem::path parent =
        Utf8Path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    strncpy_s(s_crashDumpPath, sizeof(s_crashDumpPath), path.c_str(), _TRUNCATE);
}

static LONG WINAPI CrashDumpHandler(EXCEPTION_POINTERS* info) {
    static bool s_dumped = false;
    if (s_dumped) return EXCEPTION_CONTINUE_SEARCH; // 避免递归转储
    s_dumped = true;

    FILE* f = nullptr;
    const std::filesystem::path crashPath =
        Utf8Path(s_crashDumpPath);
    _wfopen_s(&f, crashPath.c_str(), L"w");
    if (f) {
        fprintf(f, "=== EngineMain crash dump ===\n");
        fprintf(f, "Exception code : 0x%08X\n", info->ExceptionRecord->ExceptionCode);
        fprintf(f, "Exception addr : %p\n", (void*)info->ExceptionRecord->ExceptionAddress);
        fprintf(f, "Module base    : %p (exe)\n", (void*)GetModuleHandleA(nullptr));
        // 模块列表（用于把栈帧地址归属到具体模块）
        HMODULE mods[256] = {};
        DWORD cbNeeded = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cbNeeded)) {
            int n = (int)(cbNeeded / sizeof(HMODULE));
            fprintf(f, "Loaded modules : %d\n", n);
            for (int i = 0; i < n; ++i) {
                char name[MAX_PATH] = {};
                GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH);
                MODULEINFO mi = {};
                GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
                fprintf(f, "  MOD %-24s base=%p size=0x%08X\n",
                    name, mi.lpBaseOfDll, (unsigned)mi.SizeOfImage);
            }
        }
        // 调用栈回溯
        void* stack[40] = {};
        USHORT frames = CaptureStackBackTrace(0, 40, stack, nullptr);
        fprintf(f, "Stack frames   : %u\n", frames);

        // Game.dll 基址（栈帧归属用）
        HMODULE gameMod = GetModuleHandleA("Game.dll");
        ULONG_PTR gameBase = gameMod ? (ULONG_PTR)gameMod : 0;
        HMODULE vcMod = GetModuleHandleA("VCRUNTIME140.dll");
        ULONG_PTR vcBase = vcMod ? (ULONG_PTR)vcMod : 0;
        fprintf(f, "Game.dll base  : %p\n", (void*)gameBase);
        fprintf(f, "VCRUNTIME base : %p\n", (void*)vcBase);

        for (USHORT i = 0; i < frames; ++i) {
            ULONG_PTR addr = (ULONG_PTR)stack[i];
            fprintf(f, "  [%02u] %p", i, (void*)addr);
            if (gameBase && addr >= gameBase && addr < gameBase + 0x1000000ULL)
                fprintf(f, "  Game.dll+0x%llX", (unsigned long long)(addr - gameBase));
            if (vcBase && addr >= vcBase && addr < vcBase + 0x100000ULL)
                fprintf(f, "  VCRUNTIME+0x%llX", (unsigned long long)(addr - vcBase));
            fprintf(f, "\n");
        }
        // 崩溃现场不只有栈，还有"出事前发生了什么"。取环形缓冲最近若干条日志：
        // 它比 engine.log 可靠 —— 文件那边非 Error 级别不 flush，崩溃时尾部可能缺行。
        // 120 条足以覆盖一帧到一次完整初始化，又不至于把 dump 写爆。
        // 该函数内部用 try_lock，拿不到锁就写一行说明后返回，不会挂死。
        Core::WriteRecentLogsRaw(f, 120);
        fclose(f);
        fprintf(stderr, "[Crash] exception 0x%08X at %p, dump -> crash_log.txt\n",
            info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH; // 继续走系统默认崩溃流程
}
#endif
// 场景渲染器
#include "SceneRenderer.h"

// 2D Canvas 渲染核心初始化
#include "SkyboxRenderer.h"
#include "AtmosphereRenderer.h"
#include "RenderTarget.h"
#include "FullscreenQuad.h"
#include "EngineConfig.h"

// 物理系统
#include "PhysicsManager.h"
#include "ECS/PhysicsSystem.h"

// 体素世界（从 OpenGL 版迁移）
#include "ECS/Systems/WorldSystem.h"
#include "World/WorldGlobals.h"
#include "World/World.h"
#include "AABB.h"

// 音频系统
#include "AudioManager.h"

// 全局变量声明
extern SceneRenderer g_SceneRenderer;
extern SkyboxRenderer g_SkyboxRenderer;
extern RenderTarget g_SceneRenderTarget;
extern RenderTarget g_GameRenderTarget;
extern FullscreenQuad g_FullscreenQuad;
extern FullscreenQuad g_SceneCompositeQuad;
extern FullscreenQuad g_GameCompositeQuad;
extern FullscreenQuad g_SceneFilterQuad;
extern FullscreenQuad g_GameFilterQuad;
extern InfiniteGridRenderer g_InfiniteGridRenderer;
#include "Rendering/PostProcessChain.h"
extern PostProcessChain g_SceneChain;
extern PostProcessChain g_GameChain;
extern PostProcessChain g_SwapChain;
extern AtmosphereRenderer g_AtmosphereRenderer;
extern bool g_AtmosphereEnabled;
extern VkRenderPass g_CompositeRenderPass;  // 游戏模式合并 render pass（VulkanManager 定义）
extern VkRenderPass g_CompositeUIPass;      // 游戏模式 swapchain UI 叠加 pass（VulkanManager 定义）
extern SDL_Window* window;
extern std::chrono::time_point<std::chrono::high_resolution_clock> g_LastTime;
extern bool g_IsPaused;

// 物理系统
extern Physics::PhysicsManager g_PhysicsManager;
extern std::shared_ptr<ECS::PhysicsSystem> g_PhysicsSystemPtr;

// 体素世界系统
extern std::shared_ptr<ECS::WorldSystem> g_WorldSystemPtr;
extern World* g_World;

// 相机锁定目标（用于相机跟随）
ECS::Entity cameraLockedEntity = ECS::INVALID_ENTITY;

namespace {

using EngineCpuProfileClock = std::chrono::steady_clock;

bool IsEngineCpuProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MIKAN_CPU_PROFILE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

uint64_t g_engineCpuProfileFrames = 0;
double g_engineCpuProfileTotalMs = 0.0;
double g_engineCpuProfileUpdateMs = 0.0;
double g_engineCpuProfileRenderMs = 0.0;
double g_engineCpuProfilePresentMs = 0.0;
double g_engineCpuProfileEditorUiMs = 0.0;

void RecordEngineCpuProfile(EngineCpuProfileClock::time_point frameStart,
                            EngineCpuProfileClock::time_point renderStart,
                            EngineCpuProfileClock::time_point afterRender,
                            EngineCpuProfileClock::time_point afterPresent,
                            double editorUiMs) {
    ++g_engineCpuProfileFrames;
    const auto milliseconds = [](EngineCpuProfileClock::time_point begin,
                                 EngineCpuProfileClock::time_point end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    g_engineCpuProfileTotalMs += milliseconds(frameStart, afterPresent);
    g_engineCpuProfileUpdateMs += milliseconds(frameStart, renderStart);
    g_engineCpuProfileRenderMs += milliseconds(renderStart, afterRender);
    g_engineCpuProfilePresentMs += milliseconds(afterRender, afterPresent);
    g_engineCpuProfileEditorUiMs += editorUiMs;

    if ((g_engineCpuProfileFrames % 60u) == 0u) {
        const double invFrames = 1.0 / static_cast<double>(g_engineCpuProfileFrames);
        LOGI("[EngineMain][CPU] frames=%llu avg_frame_ms=%.3f avg_update_ms=%.3f avg_render_ms=%.3f avg_present_ms=%.3f avg_editor_ui_ms=%.3f",
               static_cast<unsigned long long>(g_engineCpuProfileFrames),
               g_engineCpuProfileTotalMs * invFrames,
               g_engineCpuProfileUpdateMs * invFrames,
               g_engineCpuProfileRenderMs * invFrames,
               g_engineCpuProfilePresentMs * invFrames,
               g_engineCpuProfileEditorUiMs * invFrames);
    }
}

} // namespace

#ifdef __ANDROID__
// SDL 的 Android 窗口由 Java SurfaceView 异步提供。息屏/唤醒或切换刷新率时，
// SDL_Window 仍然存在，但其 ANativeWindow 可能短暂为空；Adreno 驱动在这种情况下
// 会在 vkCreateAndroidSurfaceKHR 内部直接解引用空指针，而不是返回错误。
static bool AndroidWindowSurfaceReady(SDL_Window* windowHandle)
{
    if (windowHandle == nullptr) return false;
    const SDL_PropertiesID properties = SDL_GetWindowProperties(windowHandle);
    return properties != 0 &&
           SDL_GetPointerProperty(properties,
                                  SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                  nullptr) != nullptr;
}

static bool WaitForAndroidWindowSurface(SDL_Window* windowHandle, Uint32 timeoutMs = 3000)
{
    const Uint64 deadline = SDL_GetTicks() + timeoutMs;
    while (!AndroidWindowSurfaceReady(windowHandle)) {
        if (SDL_GetTicks() >= deadline) return false;
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    return true;
}

// Android Activity 在 Vulkan 初始化期间覆盖一张原生 Logo，避免 SurfaceView 已创建但
// 引擎尚未能提交第一帧时出现数秒黑屏。进入引擎 Splash 后立即移除覆盖层。
static void HideAndroidNativeSplashOverlay()
{
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) return;

    jclass activityClass = env->GetObjectClass(activity);
    if (activityClass == nullptr) return;
    jmethodID hideMethod = env->GetMethodID(activityClass, "hideNativeSplash", "()V");
    if (hideMethod != nullptr) {
        env->CallVoidMethod(activity, hideMethod);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(activityClass);
}

static bool CreateAndroidVulkanSurface(SDL_Window* windowHandle,
                                       VkInstance instance,
                                       VkAllocationCallbacks* allocator,
                                       VkSurfaceKHR* surface)
{
    if (surface != nullptr) *surface = VK_NULL_HANDLE;
    if (!WaitForAndroidWindowSurface(windowHandle)) {
        LOGE("[Android] ANativeWindow is not ready; skip Vulkan surface creation");
        return false;
    }
    return SDL_Vulkan_CreateSurface(windowHandle, instance, allocator, surface);
}

// android/sync_assets.ps1 writes the selected project's manifest scene here.
// Keep a source-tree fallback only for local APK experiments; the canonical
// Android package scene is supplied by the selected project, never by the
// repository root assets/ directory.
static std::string ReadAndroidStartupScenePath()
{
    constexpr const char* kFallbackScene = "scenes/main.json";
    SDL_IOStream* io = SDL_IOFromFile("mikan_android_scene.txt", "rb");
    if (io == nullptr) {
        LOGW("[Android] Startup scene manifest unavailable, using %s", kFallbackScene);
        return kFallbackScene;
    }

    const Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0 || fileSize > 4096) {
        SDL_CloseIO(io);
        LOGE("[Android] Startup scene manifest invalid, using %s", kFallbackScene);
        return kFallbackScene;
    }

    std::string scenePath(static_cast<size_t>(fileSize), '\0');
    const size_t bytesRead = SDL_ReadIO(io, scenePath.data(), static_cast<size_t>(fileSize));
    SDL_CloseIO(io);
    if (bytesRead != static_cast<size_t>(fileSize)) {
        LOGE("[Android] Startup scene manifest read failed, using %s", kFallbackScene);
        return kFallbackScene;
    }

    const size_t first = scenePath.find_first_not_of(" \t\r\n");
    const size_t last = scenePath.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return kFallbackScene;
    scenePath = scenePath.substr(first, last - first + 1);
    if (scenePath.empty() || scenePath.find("../") == 0 || scenePath.find("..\\") == 0) {
        LOGI("[Android] Startup scene path escapes the APK asset root, using %s", kFallbackScene);
        return kFallbackScene;
    }
    return scenePath;
}
#endif

// ==== 辅助函数 ====

// 编辑器播放态使用内存快照隔离运行时修改。停止时恢复快照，既能撤销
// Transform/物理/UI 等运行时状态，也不会覆盖编辑器中尚未保存的改动。
static std::string s_editorPlaySnapshot;
static bool s_editorPlaySnapshotValid = false;

static void CaptureEditorPlaySnapshot()
{
    ECS::SceneSerializer serializer;
    s_editorPlaySnapshot = serializer.SerializeScene();
    s_editorPlaySnapshotValid = !s_editorPlaySnapshot.empty();
    if (s_editorPlaySnapshotValid) {
        LOGI("[EditorPlayMode] Captured scene snapshot before play (%zu bytes)",
               s_editorPlaySnapshot.size());
    } else {
        LOGE("[EditorPlayMode] Failed to capture scene snapshot before play");
    }
}

static bool RestoreEditorPlaySnapshot()
{
    if (!s_editorPlaySnapshotValid) {
        LOGI("[EditorPlayMode] No scene snapshot available for stop");
        return false;
    }

    // 先清理场景外部持有的运行时对象，再让反序列化清理/重建 ECS 实体。
    Physics2DSystem::GetInstance().ClearBodies();
    TilemapSystem::GetInstance().ClearAll();

    ECS::SceneSerializer serializer;
    if (!serializer.DeserializeScene(s_editorPlaySnapshot)) {
        LOGE("[EditorPlayMode] Failed to restore scene snapshot");
        return false;
    }

    // 场景实体已换新，所有按实体 ID 保存的相机/瓦片运行时状态都必须重建。
    Camera2DSystem::GetInstance().Reset();
    Camera2DSystem::GetInstance().Rebind();
    ThirdPersonCameraSystem::GetInstance().Reset();
    TilemapSystem::GetInstance().LoadAllFromScene();

    // 反序列化会恢复场景关联的游戏名；重新通知模块绑定新的实体句柄。
    // 场景通常不重复保存 project.json 的 game 字段，编辑器停止播放后
    // 必须回退到当前项目 manifest，否则 GameManager 会被误清空，导致
    // 粒子/布料等由游戏模块驱动的渲染对象在编辑态消失。
    const std::string& sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
    std::string restoreGame = sceneGame;
    if (restoreGame.empty() && ProjectManager::GetInstance().HasManifest()) {
        restoreGame = ProjectManager::GetInstance().GetManifest().game;
    }
    auto& gameManager = Game::GameManager::GetInstance();
    Game::IGameModule* game = gameManager.GetCurrent();
    if (restoreGame.empty() && game != nullptr && game->GetName() != nullptr) {
        restoreGame = game->GetName();
    }
    if (restoreGame.empty()) {
        gameManager.Deactivate();
    } else {
        const char* currentGameName = game ? game->GetName() : nullptr;
        if (!game || currentGameName == nullptr ||
            restoreGame != currentGameName) {
            game = gameManager.Activate(restoreGame);
        }
        if (game) {
            game->OnSceneLoaded();
        } else {
            LOGE("[EditorPlayMode] Failed to reactivate game module: %s",
                   restoreGame.c_str());
        }
    }
    ECS::ScriptSystem::GetInstance().InstantiateAll(false);

    LOGI("[EditorPlayMode] Restored scene snapshot after stop");
    return true;
}









// ==== 主函数 ====

// ==================== Editor.dll bridge (runtime loads editor if present) ====================
#ifdef _WIN32
static HMODULE s_editorDll = nullptr;
static bool s_editorActive = false;
typedef bool (*EditorAttachFn)(SDL_Window*, int, int, float);
typedef void (*EditorRenderFrameFn)();
typedef void (*EditorDetachFn)();
typedef bool (*EditorQueryFn)();
typedef void (*EditorSetDescFn)(VkDescriptorSet);
static EditorAttachFn s_editorAttach = nullptr;
static EditorRenderFrameFn s_editorRenderFrame = nullptr;
static EditorDetachFn s_editorDetach = nullptr;
static EditorQueryFn s_editorIsGameRunning = nullptr;
static EditorQueryFn s_editorIsGamePaused = nullptr;
static EditorSetDescFn s_editorSetSceneViewDesc = nullptr;
static EditorSetDescFn s_editorSetGameViewDesc = nullptr;

static bool DetectEditorDll()
{
    // Resolve beside the host executable first. The project manager can be
    // launched from an arbitrary working directory, so a bare relative
    // LoadLibraryA("Editor.dll") makes the editor appear to be unavailable
    // even when the release bundle contains Editor.dll.
    std::string editorPath = "Editor.dll";
    if (const char* basePath = SDL_GetBasePath(); basePath && basePath[0] != '\0') {
        editorPath = std::string(basePath) + "Editor.dll";
    }
#ifdef _WIN32
    const std::wstring editorWidePath = mikanpath::Utf8ToWide(editorPath);
    s_editorDll = LoadLibraryW(editorWidePath.c_str());
#else
    s_editorDll = LoadLibraryA(editorPath.c_str());
#endif
    if (!s_editorDll) {
        const DWORD loadError = GetLastError();
        LOGE("[Editor] LoadLibrary failed: %s (error=%lu)",
                editorPath.c_str(), static_cast<unsigned long>(loadError));
        return false;
    }
    s_editorAttach = (EditorAttachFn)GetProcAddress(s_editorDll, "MikanEditor_Attach");
    s_editorRenderFrame = (EditorRenderFrameFn)GetProcAddress(s_editorDll, "MikanEditor_RenderFrame");
    s_editorDetach = (EditorDetachFn)GetProcAddress(s_editorDll, "MikanEditor_Detach");
    s_editorIsGameRunning = (EditorQueryFn)GetProcAddress(s_editorDll, "MikanEditor_IsGameRunning");
    s_editorIsGamePaused = (EditorQueryFn)GetProcAddress(s_editorDll, "MikanEditor_IsGamePaused");
    s_editorSetSceneViewDesc = (EditorSetDescFn)GetProcAddress(s_editorDll, "MikanEditor_SetSceneViewDescriptor");
    s_editorSetGameViewDesc = (EditorSetDescFn)GetProcAddress(s_editorDll, "MikanEditor_SetGameViewDescriptor");
    const bool valid = s_editorAttach && s_editorRenderFrame && s_editorDetach;
    if (!valid) {
        LOGE("[Editor] Editor.dll is missing required exports");
        FreeLibrary(s_editorDll);
        s_editorDll = nullptr;
        s_editorAttach = nullptr;
        s_editorRenderFrame = nullptr;
        s_editorDetach = nullptr;
    }
    return valid;
}

static bool AttachEditor(SDL_Window* window, int w, int h, float scale)
{
    if (!s_editorDll) return false;
    s_editorActive = s_editorAttach(window, w, h, scale);
    g_EditorActive = s_editorActive;
    return s_editorActive;
}

static void ShutdownEditorDll()
{
    if (s_editorActive && s_editorDetach)
        s_editorDetach();
    s_editorActive = false;
    g_EditorActive = false;
    if (s_editorDll)
        FreeLibrary(s_editorDll);
    s_editorDll = nullptr;
}
#endif

// 导出给 Editor.dll 项目管理器:选择项目后切换项目根并加载其场景(启动页模式)
// 定义于 Game.dll 的 EngineGlobals.cpp(Editor.dll 链接 Game.lib;此处仅 exe 侧不重复定义)

// Game.dll 导出（EngineMain.exe 链接 Game.lib 调用）
#ifdef _WIN32
extern "C" __declspec(dllimport) bool MikanEngine_OpenProject(const char* dir);
#else
extern "C" bool MikanEngine_OpenProject(const char* dir);
#endif

#include "EngineRuntimeLoop.inl"
#include "EngineSceneStartup.inl"

#ifdef __ANDROID__
int main(int argc, char* argv[]) {
#else
extern "C" __declspec(dllexport) int MikanEngineMain(int argc, char* argv[]) {
#endif
    // Windows 下设置控制台 UTF-8 编码，支持中文输出
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    ConfigureCrashDumpPath(argc, argv);
    SetUnhandledExceptionFilter(CrashDumpHandler); // 崩溃时写 crash_log.txt
    // 代码页（GBK），遇映射不了的 Unicode 文件名（emoji/生僻字）抛 "没有从 Unicode 字符映射到
    // 当前页"（ERROR_NO_UNICODE_TRANSLATION，AssetsWindow 扫描报错、中文路径加载失败）。
    // 全局 locale + filesystem imbue 后：path::string()/name 输出 UTF-8，filesystem 迭代不抛。
    // （注：MSVC STL 无 path::imbue，path::string() 直接受全局 locale codecvt 影响，设全局即可）
    {
        std::locale utf8loc(std::locale::classic(), new std::codecvt_utf8_utf16<wchar_t>);
        std::locale::global(utf8loc);
    }
#endif
    
    // Detect the engine install root and optionally select --project. Desktop
    // startup never promotes the engine checkout to an implicit project.
    if (!ProjectManager::GetInstance().Initialize(argc, argv)) {
        LOGE("[Startup] ERROR: failed to initialize engine/project paths");
        return 2;
    }
    const char* buildType =
#ifdef NDEBUG
        "Release";
#else
        "Debug";
#endif
    LOGI("==== MikanEngine starting (build %s) ====", buildType);

    Core::EngineCommandLineOptions commandLine;
    Core::ParseEngineCommandLine(argc, argv, commandLine);
    if (commandLine.disableVoxelWorld) {
        g_EnableVoxelWorld = false;
    }
    if (commandLine.enableZPrepass) {
        g_EnableZPrepass = true;
    }
    if (commandLine.logDebug) {
        Core::SetLogLevel(Core::LogLevel::Debug);
    }
    if (commandLine.cpuSkinning) {
        g_UseGpuSkinning = false;
        LOGW("GPU skinning disabled -> CPU skinning fallback");
    }

    bool skipProjectManager = commandLine.skipProjectManager;
    bool forceGameMode = commandLine.forceGameMode;
    const bool headless = commandLine.headless;
    const bool headlessEditor = commandLine.headlessEditor;
    const bool headlessNoRender = commandLine.headlessNoRender;
    const bool prefabSelftest = commandLine.prefabSelftest;
    const bool physics2dSelftest = commandLine.physics2dSelftest;
    int headlessFrames = commandLine.headlessFrames;
    float fixedDeltaSeconds = commandLine.fixedDeltaSeconds;
    const int renderDocCaptureFrame = commandLine.renderDocCaptureFrame;
    const std::string renderDocCapturePath = commandLine.renderDocCapturePath;
    const int screenshotFrame = commandLine.screenshotFrame;
    const std::string screenshotPath = commandLine.screenshotPath;
    const std::string dumpStatePath = commandLine.dumpStatePath;
    const std::string dumpSchemaPath = commandLine.dumpSchemaPath;
    const std::string sceneArg = commandLine.sceneArg;
    const std::string gameArg = commandLine.gameArg;
    if (commandLine.invalidArguments) return 64;
    if (renderDocCaptureFrame > 0 && headlessNoRender) {
        LOGE("[RenderDoc] ERROR: capture requires a rendered/presented frame; --headless-no-render is incompatible");
        return 64;
    }
    if (screenshotFrame > 0 && headlessNoRender) {
        LOGE("[Screenshot] ERROR: capture requires a rendered/presented frame; --headless-no-render is incompatible");
        return 64;
    }
    if (headless && fixedDeltaSeconds <= 0.0f) {
        fixedDeltaSeconds = 1.0f / 60.0f;
    }
    Core::RenderDocCapture::GetInstance().Configure(renderDocCaptureFrame, renderDocCapturePath);
    Core::RenderDocCapture::GetInstance().Initialize();
    Core::ScreenshotCapture::GetInstance().Configure(screenshotFrame, screenshotPath);
#ifdef __ANDROID__
    // Android 几何直通模式：体素世界（WorldRenderer 管线创建在 Adreno 上崩）整体关闭，先保证应用启动
    g_EnableVoxelWorld = false;
    LOGI("Voxel world disabled on Android (WorldRenderer Adreno pipeline crash workaround)");
#endif
    if (!g_EnableVoxelWorld) {
        LOGI("Voxel world disabled (--no-voxel-world)");
    }
    if (skipProjectManager) {
        LOGW("Project manager skipped (--no-project-manager)");
    }
    if (headless) {
        if (!headlessEditor) {
            forceGameMode = true;  // 普通 headless 不加载编辑器，走纯游戏渲染路径
        }
        LOGI("Headless mode enabled (frames=%d, fixed_dt=%.6f, render=%s, editor=%s)",
            headlessFrames, fixedDeltaSeconds, headlessNoRender ? "off" : "on",
            headlessEditor ? "on" : "off");
    }
    // 纯游戏模式(--no-editor)/headless: 编辑器未加载,项目管理器启动页无人渲染，
    // 因此自动跳过 UI；项目根仍必须来自显式 --project。
    if (forceGameMode) {
        skipProjectManager = true;
        LOGW("Game mode: auto-skipping project manager (standalone run)");
    }

#ifndef __ANDROID__
    if ((forceGameMode || skipProjectManager) &&
        !ProjectManager::GetInstance().HasActiveProject()) {
        LOGE(
                "[Startup] ERROR: desktop game/headless mode requires --project <project-directory>");
        return 2;
    }
#endif

    // --dump-schema <path>: 导出组件 schema JSON 后立即退出（不初始化窗口/Vulkan）
    if (!dumpSchemaPath.empty()) {
        ECS::RegisterAllComponentMeta();
        Core::DumpSchema(dumpSchemaPath);
        return 0;
    }
    if (headless && headlessFrames <= 0 && !prefabSelftest && !physics2dSelftest) {
        LOGE("[Headless] ERROR: --headless requires --frames N (N >= 1) unless a self-test is selected");
        return 64;
    }

    // ===== 引擎必需资源校验（A 类引擎资产 + B 类编辑器资产；游戏可选资源不在此列）=====
    {
        const std::vector<std::string> missingAssets = EngineAssets::ValidateEngineAssets();
        if (!missingAssets.empty()) {
            LOGE("==== Engine assets missing: %zu ====", missingAssets.size());
            for (const auto& m : missingAssets) {
                LOGE("  MISSING: %s", m.c_str());
            }
            if (headless) {
                LOGE("[Headless] engine assets incomplete, aborting (exit=2)");
                return 2;
            }
        } else {
            LOGI("Engine assets OK (all required files present)");
        }
    }

#ifdef _WIN32
    // 桌面端用原生覆盖层填补 SDL/Vulkan 初始化前的首帧空窗。窗口尺寸和
    // 位置在计算出引擎普通窗口参数后再传入；首帧 Vulkan Logo 提交成功后
    // 由 renderStartupSplash() 关闭。headless 不创建覆盖层。
    DesktopStartupSplashOverlay desktopStartupSplash;
#endif

    // 初始化 SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        LOGE("Error: SDL_Init(): %s", SDL_GetError());
        return 1;
    }
    
    LOGI("SDL initialized successfully");

    // SDL3_image 3.0 不需要手动初始化

    // 主显示器（用于初始窗口定位/裁剪）。编辑器 UI 缩放不在这里取——窗口创建后
    // 按窗口实际所在显示器计算（见 AttachEditor 调用处），避免多显示器设备上
    // 主/副屏 DPI 不一致，也不再乘 0.7 经验系数。
    SDL_DisplayID display = SDL_GetPrimaryDisplay();

    // 普通窗口启动：按显示器可用区域裁剪初始客户区，保留窗口标题栏/边框，绝不切换全屏。
    const EngineDisplaySettings& displaySettings =
        ProjectManager::GetInstance().GetEngineDisplaySettings();
    int initialWindowWidth = displaySettings.engineWidth;
    int initialWindowHeight = displaySettings.engineHeight;
    #ifndef __ANDROID__
    SDL_Rect initialDisplayBounds{};
    bool hasInitialDisplayBounds = false;
    if (display != 0)
    {
        if (SDL_GetDisplayUsableBounds(display, &initialDisplayBounds) &&
            initialDisplayBounds.w > 0 && initialDisplayBounds.h > 0)
        {
            // SDL_CreateWindow 的宽高是客户区尺寸；为 Windows 非客户区预留保守空间。
            constexpr int kWindowChromeWidth = 16;
            constexpr int kWindowChromeHeight = 40;
            const int maxClientWidth = std::max(640, initialDisplayBounds.w - kWindowChromeWidth);
            const int maxClientHeight = std::max(360, initialDisplayBounds.h - kWindowChromeHeight);
            initialWindowWidth = std::min(initialWindowWidth, maxClientWidth);
            initialWindowHeight = std::min(initialWindowHeight, maxClientHeight);
            hasInitialDisplayBounds = true;
            LOGI("[Window] initial windowed client size: %dx%d (usable display: %dx%d)",
                   initialWindowWidth, initialWindowHeight,
                   initialDisplayBounds.w, initialDisplayBounds.h);
        }
    }
    #endif

#ifdef _WIN32
    if (!headless && !headlessNoRender) {
        const int splashX = hasInitialDisplayBounds ? initialDisplayBounds.x : 0;
        const int splashY = hasInitialDisplayBounds ? initialDisplayBounds.y : 0;
        desktopStartupSplash.Show(
            ProjectManager::GetInstance().GetEngineAssetPath("ui/mikan_engine_splash.png"),
            initialWindowWidth, initialWindowHeight, splashX, splashY);
    }
#endif
    
    // 创建窗口
    SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    #ifndef __ANDROID__
    window_flags |= SDL_WINDOW_RESIZABLE;
    #endif
    if (headless) window_flags |= SDL_WINDOW_HIDDEN;  // headless: 不显示窗口
    window = SDL_CreateWindow(EngineConfig::WINDOW_TITLE, initialWindowWidth, initialWindowHeight, window_flags);
    if (window == nullptr)
    {
        LOGE("Error: SDL_CreateWindow(): %s", SDL_GetError());
        return 1;
    }
    #ifndef __ANDROID__
    if (hasInitialDisplayBounds)
    {
        // SDL 的定位基准是客户区；补偿真实非客户区，确保标题栏位于屏幕内。
        int borderTop = 0;
        int borderLeft = 0;
        int borderBottom = 0;
        int borderRight = 0;
        SDL_SyncWindow(window);
        const bool haveBorders = SDL_GetWindowBordersSize(
            window, &borderTop, &borderLeft, &borderBottom, &borderRight);
        if (haveBorders)
        {
            SDL_SetWindowPosition(window,
                                  initialDisplayBounds.x + borderLeft,
                                  initialDisplayBounds.y + borderTop);
        }
        else
        {
            // 某些窗口系统在创建瞬间无法报告边框，仍先保证不使用负坐标。
            SDL_SetWindowPosition(window, initialDisplayBounds.x, initialDisplayBounds.y);
        }
    }
    #endif
    g_InputController.SetWindow(window);

    // 获取SDL需要的Vulkan扩展
    ImVector<const char*> extensions;
    {
        uint32_t sdl_extensions_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extensions_count);
        if (sdl_extensions != nullptr)
        {
            for (uint32_t n = 0; n < sdl_extensions_count; n++)
                extensions.push_back(sdl_extensions[n]);
        }
    }
    
    // 初始化Vulkan
    ::SetupVulkan(extensions);

    // 创建Vulkan表面
    VkSurfaceKHR surface;
    VkResult err;
    #ifdef __ANDROID__
    const bool surfaceCreated = CreateAndroidVulkanSurface(window, g_Instance, g_Allocator, &surface);
    #else
    const bool surfaceCreated = SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &surface);
    #endif
    if (!surfaceCreated)
    {
        LOGE("Failed to create Vulkan surface.");
        return 1;
    }

    // 获取窗口大小
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    
    // 设置Vulkan窗口
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    ::SetupVulkanWindow(wd, surface, w, h);

    // 初始化ImGui
    // Runtime texture pool (renderers use this; the editor attaches its own pool)
    g_TexturePool = new TexturePool(g_Device, g_PhysicalDevice, g_CommandPool, g_Queue, g_Allocator);

    // Editor runtime settings (Editor.dll syncs them each frame when present)
    g_ShowSceneView = false;
    g_ShowGameView = false;

    // 检测 Editor.dll(存在则编辑器模式;--no-editor 强制纯游戏模式,即使 Editor.dll 存在)
    const bool forceSelftest = physics2dSelftest;
    bool editorActive = false;
#ifdef _WIN32
    if (!forceGameMode) {
        editorActive = DetectEditorDll();
    } else {
        LOGI("Editor disabled (--no-editor), forcing game mode");
    }
#endif
    if (editorActive)
        LOGI("Editor.dll loaded - editor mode");
    else {
        // Standalone game build: no editor -> force game mode. A project is
        // still mandatory; there is no registered/default project fallback.
        g_RunMode = RunMode::Game;
        forceGameMode = true;
        skipProjectManager = true;
        LOGE("Editor.dll not found - running without editor (game mode)");
#ifndef __ANDROID__
        if (!ProjectManager::GetInstance().HasActiveProject()) {
            LOGE(
                    "[Startup] ERROR: Editor.dll is unavailable and no project was selected; pass --project <project-directory>");
            return 2;
        }
#endif
    }
    
    // 初始化ECS场景管理器
    ECS::SceneECS::GetInstance().Init();
    LOGI("SceneECS initialized successfully");
    
    // 注册物理系统到 ECS
    auto& coordinator = ECS::Coordinator::GetInstance();
    g_PhysicsSystemPtr = coordinator.RegisterSystem<ECS::PhysicsSystem>();
    {
        ECS::Signature signature;
        signature.set(coordinator.GetComponentType<ECS::TransformComponent>());
        signature.set(coordinator.GetComponentType<ECS::RigidBodyComponent>());
        coordinator.SetSystemSignature<ECS::PhysicsSystem>(signature);
    }
    LOGI("PhysicsSystem registered to ECS");
    
    // 初始化物理系统
    g_PhysicsSystemPtr->SetPhysicsManager(&g_PhysicsManager);
    g_PhysicsSystemPtr->Initialize();
    LOGI("PhysicsSystem initialized successfully");
    
    // 初始化 2D 物理(Box2D)
    Physics2DSystem::GetInstance().Initialize();

    // ===== 2D 物理自测(--phys2d-selftest): 不渲染/不依赖 UI, 直接验证重力下落 =====
    if (forceSelftest) {
        LOGI("[SELFTEST] Starting 2D physics self-test...");
        b2WorldId world = *Physics2DManager::GetInstance().GetWorldIdPtr();
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_dynamicBody;
        bd.position = { 0.0f, 0.0f };
        b2BodyId body = b2CreateBody(world, &bd);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.density = 1.0f;
        b2Polygon poly = b2MakeBox(0.4f, 0.4f); // 0.8m 箱子
        b2CreatePolygonShape(body, &sd, &poly);
        for (int i = 0; i <= 120; ++i) {
            b2World_Step(world, 1.0f / 60.0f, 4);
            if (i % 20 == 0) {
                b2Vec2 p = b2Body_GetPosition(body);
                LOGI("[SELFTEST] frame=%d pos=(%.3f, %.3f) mass=%.2f",
                        i, p.x, p.y, b2Body_GetMass(body));
            }
        }
        b2DestroyBody(body);
        b2DestroyWorld(world);
        LOGI("[SELFTEST] Done. If pos.y increased over frames -> gravity works.");
        return 0;
    }
    
    // 注册体素世界系统到 ECS（签名：WorldComponent）
    if (g_EnableVoxelWorld) {
        g_WorldSystemPtr = coordinator.RegisterSystem<ECS::WorldSystem>();
        {
            ECS::Signature signature;
            signature.set(coordinator.GetComponentType<ECS::WorldComponent>());
            coordinator.SetSystemSignature<ECS::WorldSystem>(signature);
        }
        LOGI("WorldSystem registered to ECS");
    }
    
    // 初始化音频管理器
    if (!AudioManager::GetInstance().Initialize()) {
        LOGE("Warning: Audio manager initialization failed");
    }
    
    // 初始化离屏渲染目标。视窗分辨率与实际 SDL 窗口分离，便于在编辑器
    // 中用固定渲染尺寸控制 SceneView/GameView 的质量和成本。
    LOGI("[Viewport] internal render size: %dx%d",
           displaySettings.viewportWidth, displaySettings.viewportHeight);
    g_SceneRenderTarget.Init(displaySettings.viewportWidth,
                             displaySettings.viewportHeight, true); // 启用MRT
    g_GameRenderTarget.Init(displaySettings.viewportWidth,
                            displaySettings.viewportHeight, true);  // 启用MRT
    
    // 编辑器模式：先 Attach（初始化 ImGui），再创建 ImGui 描述符集
    if (editorActive) {
#ifdef _WIN32
        // UI 缩放来源：窗口实际所在显示器的 content scale（1.0 = 100%，无经验系数）。
        // 编辑器内部以此为唯一缩放源（字体 FontScaleDpi + 样式整体同倍率缩放），
        // 运行期间由 EditorManager::UpdateUiScale 每帧跟随窗口 DPI 变化。
        float editorUiScale = SDL_GetWindowDisplayScale(window);
        if (editorUiScale <= 0.0f) {
            SDL_DisplayID windowDisplay = SDL_GetDisplayForWindow(window);
            if (windowDisplay != 0) editorUiScale = SDL_GetDisplayContentScale(windowDisplay);
        }
        if (editorUiScale <= 0.0f) editorUiScale = 1.0f;
        AttachEditor(window, w, h, editorUiScale);
#endif
        g_SceneRenderTarget.CreateImGuiDescriptorSet();
        g_GameRenderTarget.CreateImGuiDescriptorSet();
    }
    
    // 初始化场景渲染器 (使用离屏渲染目标的RenderPass)
    g_SceneRenderer.Init(g_SceneRenderTarget.GetRenderPass());
    g_SkyboxRenderer.Init(g_SceneRenderTarget.GetRenderPass());


    // 初始化全屏四边形渲染器（游戏模式合成 subpass：几何 subpass 0 + 合成 subpass 1，input attachment 读 G-Buffer）
    InitCompositeResources();
    g_InfiniteGridRenderer.Init(g_SceneRenderTarget.GetCompositeRenderPass());
    g_AtmosphereRenderer.Init(w, h);
    // 更新合成描述符：绑定真实天空 RT（必须在 AtmosphereRenderer 初始化之后）
    UpdateFullscreenQuadDescriptors();
    // 初始化 2D 渲染核心（离屏世界层 + 主窗口 UI 层；玩法随 GameView 显示, UI 叠加在主窗口）
    if (!Renderer2D::GetInstance().Init(g_GameRenderTarget.GetRenderPass(), wd->RenderPass,
                                        g_GameRenderTarget.GetDisplayUIRenderPass(), g_CompositeUIPass)) {
        LOGE("ERROR: Renderer2D init failed (shader missing?)");
    }
    LOGI("Renderer2D initialized");
    
    // 初始化文本渲染器（依赖 Renderer2D 的 descriptor layout）
    {
        std::string fontPath = EngineConfig::GetFontPath("simhei.ttf");
        if (TextRenderer::GetInstance().Init(fontPath, 24.0f)) {
            LOGI("TextRenderer initialized with %s", fontPath.c_str());
        } else {
            LOGE("WARNING: TextRenderer init failed (no font file?)");
        }
    }

    // 开屏 Logo 属于引擎系统资源，不依赖尚未选择的桌面项目。
    // Android 由 sync_assets.ps1 同步到 APK assets/ui。
    // 加载失败时仍保留文字版加载页，不阻断原型启动。
    const std::string splashLogoPath =
        ProjectManager::GetInstance().GetEngineAssetPath("ui/mikan_engine_splash.png");
    if (Renderer2D::GetInstance().LoadTexture("mikan_engine_splash", splashLogoPath)) {
        LOGI("Loading splash logo ready: %s", splashLogoPath.c_str());
    } else {
        LOGW("WARNING: Loading splash logo unavailable: %s", splashLogoPath.c_str());
    }

    // 项目管理器和独立游戏启动时，场景加载发生在主循环之前；先提交引擎级
    // 全屏 Logo，避免窗口创建后到项目管理器出现前显示黑屏。独立游戏模式
    // 在 Logo 渐隐后继续使用轻量进度加载页，编辑器模式则直接进入项目管理器。
    const bool showStartupSplash = !headless && !headlessNoRender;
    const bool showStartupLoading = forceGameMode && !editorActive && !headless;
    auto renderStartupLoading = [&](float progress, const char* status) {
        if (!showStartupLoading) return;
        SetLoadingScreenState(true, progress, status);
        const glm::mat4 identity(1.0f);
        ::FrameRender(wd, nullptr, identity, identity);
        ::FramePresent(wd);
    };

    auto renderStartupSplash = [&]() {
        if (!showStartupSplash) return;
        const glm::mat4 identity(1.0f);
        SetLoadingScreenState(true, 0.0f, "");

        // 先提交一帧与原生覆盖层完全一致的 Vulkan Logo，再移除原生层。
        // 这样原生层只负责填补首帧空窗，不会在原生 Logo 与 Vulkan Logo
        // 之间产生一次可见的尺寸跳变。
        SetStartupSplashState(true, 1.0f);
        ::FrameRender(wd, nullptr, identity, identity);
        ::FramePresent(wd);

#ifdef __ANDROID__
        HideAndroidNativeSplashOverlay();
#elif defined(_WIN32)
        desktopStartupSplash.Hide();
#endif
        SDL_Delay(16);

        // 短暂保持完整 Logo，避免启动时一闪而过。
        constexpr int kHoldFrames = 18;
        for (int i = 0; i < kHoldFrames; ++i) {
            SetStartupSplashState(true, 1.0f);
            ::FrameRender(wd, nullptr, identity, identity);
            ::FramePresent(wd);
            SDL_Delay(16);
        }

        // 渐隐到黑色后再切换到进度加载页。
        constexpr int kFadeFrames = 45;
        for (int i = 0; i < kFadeFrames; ++i) {
            const float t = static_cast<float>(i + 1) / static_cast<float>(kFadeFrames);
            SetStartupSplashState(true, 1.0f - t);
            ::FrameRender(wd, nullptr, identity, identity);
            ::FramePresent(wd);
            SDL_Delay(16);
        }
        SetStartupSplashState(false, 0.0f);
        if (!showStartupLoading) {
            // 编辑器启动页不需要进度卡片；渐隐结束后交还给正常的 ImGui 项目管理器帧。
            SetLoadingScreenState(false, 1.0f, "Ready");
        }
    };

    renderStartupSplash();
    renderStartupLoading(0.05f, "Preparing renderer...");
    
    const int sceneStartupResult = RunEngineSceneStartup(
        skipProjectManager, sceneArg, gameArg, prefabSelftest, w, h,
        renderStartupLoading);
    if (sceneStartupResult != 0) return sceneStartupResult;
    // ===== 2D Canvas 渲染核心初始化（场景中的 2D 实体由场景文件 / 编辑器添加；
    // 2D 世界层相机由场景树的 Camera2DComponent 每帧驱动,无组件时默认 (0,0,1)）=====
    {
        auto& canvas = UI::Canvas2D::GetInstance();
        canvas.SetViewport(w, h);
    }
    if (showStartupLoading) {
        SetLoadingScreenState(false, 1.0f, "Ready");
    }

    const int frameCount = RunEngineLoop(
        window, wd, w, h, headless, headlessNoRender,
        headlessFrames, fixedDeltaSeconds, editorActive);
    // ===== headless: 导出场景状态（在清理之前，保留最终状态）=====
    int finalExitCode = 0;
    if (!dumpStatePath.empty() && !Core::DumpSceneState(dumpStatePath, frameCount, g_FPS)) finalExitCode = 3;

    // 卸载编辑器（Editor.dll 内部清理 ImGui）
    // ⚠️ 必须先等 GPU 空闲：ImGui_ImplVulkan_Shutdown 会销毁 ImGui 的描述符池/
    // 字体纹理/管线，若最后一帧命令仍在 GPU 上执行就是在用销毁 → vkDeviceWaitIdle
    // 报 VK_ERROR_DEVICE_LOST（每次退出 100% 复现，probe 已定位到此处）。
    if (g_Device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(g_Device);
#ifdef _WIN32
    ShutdownEditorDll();
#endif

    return Core::ShutdownEngine(window, screenshotFrame, finalExitCode);
}

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL
Java_com_mikanengine_MikanEngineActivity_nativeOnPause(JNIEnv* env, jobject thiz) {
    // 当应用进入后台时，暂停渲染循环
    g_IsPaused = true;
    
    // 等待设备空闲，确保所有渲染操作都已完成
    if (g_Device != VK_NULL_HANDLE) {
        VkResult err = vkDeviceWaitIdle(g_Device);
        // 忽略错误，因为设备可能已经被销毁
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mikanengine_MikanEngineActivity_nativeOnResume(JNIEnv* env, jobject thiz) {
    // 当应用从后台恢复时，确保Vulkan设备仍然有效
    g_IsPaused = false;
    
    // 检查必要的变量是否有效
    if (window == nullptr || g_Instance == VK_NULL_HANDLE) {
        return;
    }
    
    // 重新创建Vulkan表面，因为旧的surface可能已经被销毁
    VkSurfaceKHR newSurface;
    if (CreateAndroidVulkanSurface(window, g_Instance, g_Allocator, &newSurface)) {
        // 等待设备空闲
        if (g_Device != VK_NULL_HANDLE) {
            VkResult err = vkDeviceWaitIdle(g_Device);
            // 忽略错误
        }
        
        // 清理旧的Vulkan窗口资源
        ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
        
        // 获取窗口大小
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        
        // 设置新的Vulkan窗口
        ::SetupVulkanWindow(&g_MainWindowData, newSurface, w, h);
        
        // 重建交换链
        g_SwapChainRebuild = true;
    } else {
        // SDL_Vulkan_CreateSurface 失败
        LOGE("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
    }
}
#endif
