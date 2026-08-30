
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include "Core/Log.h"
#include "Core/EngineAssets.h"
#include "Core/GameplayRuntime.h"
#include "json.hpp"
#include <fstream>
#include "Camera.h"
#include "InputController.h"
#include "ECS/SceneECS.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ScriptSystem.h"
#include <cstring>
#include "SceneSerializer.h"
#include "Core/ProjectManager.h"
#include <filesystem>
#include <locale>
#include <codecvt>
#include "Rendering/SceneRenderer.h"
#include "Rendering/ParticleSystem.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/FontAtlas.h"
#include "Rendering/TextRenderer.h"
#include "UI/Canvas2D.h"
#include "UI/TweenSystem.h"
#include "ECS/Systems/SpriteAnimatorSystem.h"
#include "ECS/Systems/AudioSourceSystem.h"
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
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

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
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
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
        std::filesystem::u8path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    strncpy_s(s_crashDumpPath, sizeof(s_crashDumpPath), path.c_str(), _TRUNCATE);
}

static LONG WINAPI CrashDumpHandler(EXCEPTION_POINTERS* info) {
    static bool s_dumped = false;
    if (s_dumped) return EXCEPTION_CONTINUE_SEARCH; // 避免递归转储
    s_dumped = true;

    FILE* f = nullptr;
    const std::filesystem::path crashPath =
        std::filesystem::u8path(s_crashDumpPath);
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
        printf("[EditorPlayMode] Captured scene snapshot before play (%zu bytes)\n",
               s_editorPlaySnapshot.size());
    } else {
        printf("[EditorPlayMode] Failed to capture scene snapshot before play\n");
    }
}

static bool RestoreEditorPlaySnapshot()
{
    if (!s_editorPlaySnapshotValid) {
        printf("[EditorPlayMode] No scene snapshot available for stop\n");
        return false;
    }

    // 先清理场景外部持有的运行时对象，再让反序列化清理/重建 ECS 实体。
    Physics2DSystem::GetInstance().ClearBodies();
    TilemapSystem::GetInstance().ClearAll();

    ECS::SceneSerializer serializer;
    if (!serializer.DeserializeScene(s_editorPlaySnapshot)) {
        printf("[EditorPlayMode] Failed to restore scene snapshot\n");
        return false;
    }

    // 场景实体已换新，所有按实体 ID 保存的相机/瓦片运行时状态都必须重建。
    Camera2DSystem::GetInstance().Reset();
    Camera2DSystem::GetInstance().Rebind();
    ThirdPersonCameraSystem::GetInstance().Reset();
    TilemapSystem::GetInstance().LoadAllFromScene();

    // 反序列化会恢复场景关联的游戏名；重新通知模块绑定新的实体句柄。
    const std::string& sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
    auto& gameManager = Game::GameManager::GetInstance();
    Game::IGameModule* game = gameManager.GetCurrent();
    if (sceneGame.empty()) {
        gameManager.Deactivate();
    } else {
        if (!game || sceneGame != game->GetName()) {
            game = gameManager.Activate(sceneGame);
        }
        if (game) {
            game->OnSceneLoaded();
        } else {
            printf("[EditorPlayMode] Failed to reactivate game module: %s\n",
                   sceneGame.c_str());
        }
    }
    ECS::ScriptSystem::GetInstance().InstantiateAll(false);

    printf("[EditorPlayMode] Restored scene snapshot after stop\n");
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
    s_editorDll = LoadLibraryA("Editor.dll");
    if (!s_editorDll)
        return false;
    s_editorAttach = (EditorAttachFn)GetProcAddress(s_editorDll, "MikanEditor_Attach");
    s_editorRenderFrame = (EditorRenderFrameFn)GetProcAddress(s_editorDll, "MikanEditor_RenderFrame");
    s_editorDetach = (EditorDetachFn)GetProcAddress(s_editorDll, "MikanEditor_Detach");
    s_editorIsGameRunning = (EditorQueryFn)GetProcAddress(s_editorDll, "MikanEditor_IsGameRunning");
    s_editorIsGamePaused = (EditorQueryFn)GetProcAddress(s_editorDll, "MikanEditor_IsGamePaused");
    s_editorSetSceneViewDesc = (EditorSetDescFn)GetProcAddress(s_editorDll, "MikanEditor_SetSceneViewDescriptor");
    s_editorSetGameViewDesc = (EditorSetDescFn)GetProcAddress(s_editorDll, "MikanEditor_SetGameViewDescriptor");
    return s_editorAttach && s_editorRenderFrame && s_editorDetach;
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

// ==================== 组件 schema 导出 ====================
// --dump-schema <path>: 遍历 ComponentRegistry 导出组件/字段 schema JSON（AI 写场景时的白名单），
// 不初始化窗口/Vulkan，毫秒级返回。组件变更后重新生成即可，永不与代码脱节。
static const char* FieldTypeName(ECS::FieldType t) {
    switch (t) {
        case ECS::FieldType::Bool:      return "Bool";
        case ECS::FieldType::Int:       return "Int";
        case ECS::FieldType::Float:     return "Float";
        case ECS::FieldType::Vec2:      return "Vec2";
        case ECS::FieldType::Vec3:      return "Vec3";
        case ECS::FieldType::Vec4:      return "Vec4";
        case ECS::FieldType::Color3:    return "Color3";
        case ECS::FieldType::Color4:    return "Color4";
        case ECS::FieldType::QuatEuler: return "QuatEuler";
        case ECS::FieldType::String:    return "String";
        case ECS::FieldType::Enum:      return "Enum";
        case ECS::FieldType::Hidden:    return "Hidden";
    }
    return "Unknown";
}

static void DumpSchema(const std::string& path) {
    FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) {
#else
    f = fopen(path.c_str(), "w");
    if (!f) {
#endif
        fprintf(stderr, "[Schema] ERROR: cannot open output: %s\n", path.c_str());
        return;
    }
    auto& reg = ECS::ComponentRegistry::GetInstance();
    const auto& all = reg.GetAll();
    fprintf(f, "{\n  \"components\": [\n");
    for (size_t ci = 0; ci < all.size(); ++ci) {
        const auto& m = all[ci];
        fprintf(f, "    {\"serializeKey\": %s, \"displayName\": \"%s\", \"category\": \"%s\", \"fields\": [",
            m.serializeKey ? (std::string("\"") + m.serializeKey + "\"").c_str() : "null",
            m.displayName ? m.displayName : "",
            m.category ? m.category : "");
        for (size_t i = 0; i < m.fieldCount; ++i) {
            const auto& fd = m.fields[i];
            fprintf(f, "%s{\"name\": \"%s\", \"type\": \"%s\"}",
                i ? ", " : "", fd.name ? fd.name : "", FieldTypeName(fd.type));
        }
        // 脚本组件的 params 是对象原文（SceneSerializer 手写序列化，不在字段表里），
        // 补入 schema 供 validate_scene 字段白名单校验通过
        if (m.serializeKey && std::strcmp(m.serializeKey, "script") == 0) {
            fprintf(f, "%s{\"name\": \"params\", \"type\": \"Object\"}",
                m.fieldCount ? ", " : "");
        }
        fprintf(f, "]}");
        if (ci + 1 < all.size()) fprintf(f, ",");
        fprintf(f, "\n");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    fprintf(stderr, "[Schema] Dumped %zu component metas -> %s\n", all.size(), path.c_str());
}

// ==================== 预制体自测（--prefab-selftest）====================
// 场景加载后：把 Baka 子树保存为预制体 -> 实例化 -> 断言实体树/组件/脚本，返回 0=通过 1=失败。
static int RunPrefabSelftest() {
    auto& scene = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    ECS::Entity baka = scene.FindByName("Baka");
    if (baka == ECS::INVALID_ENTITY) {
        printf("[PrefabSelftest] FAIL: entity 'Baka' not found in scene\n");
        return 1;
    }

    auto collectTree = [&](ECS::Entity root) -> std::vector<ECS::Entity> {
        std::vector<ECS::Entity> out;
        std::vector<ECS::Entity> stack{ root };
        while (!stack.empty()) {
            ECS::Entity e = stack.back(); stack.pop_back();
            out.push_back(e);
            for (ECS::Entity c : scene.GetChildren(e)) stack.push_back(c);
        }
        return out;
    };
    const auto origTree = collectTree(baka);

    const std::string prefabDir = ProjectManager::GetInstance().ResolveAssetPath("prefabs/");
    std::filesystem::create_directories(prefabDir);
    const std::string path = prefabDir + "Baka.prefab.json";

    ECS::SceneSerializer serializer;
    if (!serializer.SavePrefab(baka, path)) {
        printf("[PrefabSelftest] FAIL: SavePrefab\n");
        return 1;
    }
    ECS::Entity inst = serializer.InstantiatePrefab(path);
    if (inst == ECS::INVALID_ENTITY) {
        printf("[PrefabSelftest] FAIL: InstantiatePrefab\n");
        return 1;
    }
    const auto newTree = collectTree(inst);

    bool ok = (newTree.size() == origTree.size());
    // 根 transform 一致
    bool transformMatch = false;
    if (coordinator.HasComponent<ECS::TransformComponent>(inst) &&
        coordinator.HasComponent<ECS::TransformComponent>(baka)) {
        auto& t1 = coordinator.GetComponent<ECS::TransformComponent>(inst);
        auto& t2 = coordinator.GetComponent<ECS::TransformComponent>(baka);
        transformMatch = (t1.position == t2.position) && (t1.scale == t2.scale);
        ok = ok && transformMatch;
    }
    // 组件齐全（mesh/render/material/script）
    const bool hasMesh    = coordinator.HasComponent<ECS::MeshComponent>(inst);
    const bool hasRender  = coordinator.HasComponent<ECS::RenderComponent>(inst);
    const bool hasMat     = coordinator.HasComponent<ECS::MaterialComponent>(inst);
    const bool hasScript  = coordinator.HasComponent<ECS::ScriptComponent>(inst);
    ok = ok && hasMesh && hasRender && hasMat && hasScript;
    // 脚本实例已补齐（InstantiatePrefab 内部已调 InstantiateAll）
    const bool scriptInst = hasScript && (coordinator.GetComponent<ECS::ScriptComponent>(inst).runtime != nullptr);
    ok = ok && scriptInst;

    printf("[PrefabSelftest] orig_tree=%zu new_tree=%zu transform=%s mesh=%d render=%d material=%d script=%d script_inst=%d -> %s\n",
        origTree.size(), newTree.size(), transformMatch ? "match" : "DIFF",
        hasMesh ? 1 : 0, hasRender ? 1 : 0, hasMat ? 1 : 0, hasScript ? 1 : 0, scriptInst ? 1 : 0,
        ok ? "PASS" : "FAIL");

    // ===== 多实体父子树测试：构造 parent+child -> 保存 -> 实例化 -> 断言树与挂接 =====
    ECS::Entity p = scene.CreateCube("PrefabParent");
    ECS::Entity c = scene.CreateCube("PrefabChild");
    scene.SetPosition(p, glm::vec3(1.0f, 2.0f, 3.0f));
    scene.SetParent(c, p);

    const std::string treePath = prefabDir + "TreeTest.prefab.json";
    bool okTree = serializer.SavePrefab(p, treePath);
    ECS::Entity instTree = okTree ? serializer.InstantiatePrefab(treePath) : ECS::INVALID_ENTITY;
    bool treeSizeOk = false, treeChildOk = false, treePosOk = false;
    if (instTree != ECS::INVALID_ENTITY) {
        const auto newTree2 = collectTree(instTree);
        treeSizeOk = (newTree2.size() == 2);
        treeChildOk = (scene.GetChildren(instTree).size() == 1);
        treePosOk = (scene.GetPosition(instTree) == glm::vec3(1.0f, 2.0f, 3.0f));
    }
    // 清理测试实体（含子实体递归）
    {
        std::function<void(ECS::Entity)> delTree = [&](ECS::Entity e) {
            for (ECS::Entity ch : scene.GetChildren(e)) delTree(ch);
            scene.DestroyEntity(e);
        };
        delTree(p);
        if (instTree != ECS::INVALID_ENTITY) delTree(instTree);
    }
    const bool okTreeAll = okTree && instTree != ECS::INVALID_ENTITY && treeSizeOk && treeChildOk && treePosOk;
    printf("[PrefabSelftest] tree: save=%d instantiate=%d size=%d child=%d pos=%d -> %s\n",
        okTree ? 1 : 0, instTree != ECS::INVALID_ENTITY ? 1 : 0,
        treeSizeOk ? 1 : 0, treeChildOk ? 1 : 0, treePosOk ? 1 : 0,
        okTreeAll ? "PASS" : "FAIL");

    return (ok && okTreeAll) ? 0 : 1;
}

// ==================== headless 场景状态导出 ====================
// --headless --frames N --dump-state <path>: 主循环跑完 N 帧后把场景实体状态写 JSON，
// 供 AI/自动化测试断言（如"球在帧 N 时到达 (x,y)"）。退出码: 0=正常跑完, 其他=异常/崩溃。
static bool DumpSceneState(const std::string& path, int frames) {
    return Core::GameplayRuntime::DumpState(path, frames, "render-vulkan", g_FPS);
}

// Game.dll 导出（EngineMain.exe 链接 Game.lib 调用）
#ifdef _WIN32
extern "C" __declspec(dllimport) void MikanEngine_OpenProject(const char* dir);
#else
extern "C" void MikanEngine_OpenProject(const char* dir);
#endif

// 2026-08 发布版：无 Editor.dll 且未指定 --scene 时，打开 projects.json 注册的第一个项目。
// 独立可运行包直接进游戏，不停留在项目管理器启动页（启动页无人渲染 = 黑屏）。
static bool OpenFirstRegisteredProject() {
    const std::string root = ProjectManager::GetInstance().GetEngineRoot();
    std::ifstream in(std::filesystem::u8path(root + "projects.json"));
    if (!in.is_open()) {
        fprintf(stderr, "[OpenFirstRegisteredProject] cannot open %sprojects.json (root='%s')\n", root.c_str(), root.c_str());
        return false;
    }
    try {
        nlohmann::json j;
        in >> j;
        for (const auto& item : j.value("projects", nlohmann::json::array())) {
            // 2026-08 相对路径：projects.json 与引擎根同目录，相对路径拼引擎根解析
            std::string p = ProjectManager::GetInstance().ResolveProjectPath(item.value("path", ""));
            if (p.empty()) continue;
            fprintf(stderr, "[OpenFirstRegisteredProject] opening project: %s\n", p.c_str());
            ::MikanEngine_OpenProject(p.c_str()); // 读项目清单 -> 校验 assets[] -> 加载 scene -> 激活 game
            return true;
        }
        fprintf(stderr, "[OpenFirstRegisteredProject] no registered projects\n");
    } catch (const std::exception& ex) {
        fprintf(stderr, "[OpenFirstRegisteredProject] parse error: %s\n", ex.what());
    }
    return false;
}

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
    // 2026-08-17：文件系统/IO 统一 UTF-8 —— MSVC 默认 path::string()/窄串文件 IO 用系统
    // 代码页（GBK），遇映射不了的 Unicode 文件名（emoji/生僻字）抛 "没有从 Unicode 字符映射到
    // 当前页"（ERROR_NO_UNICODE_TRANSLATION，AssetsWindow 扫描报错、中文路径加载失败）。
    // 全局 locale + filesystem imbue 后：path::string()/name 输出 UTF-8，filesystem 迭代不抛。
    // （注：MSVC STL 无 path::imbue，path::string() 直接受全局 locale codecvt 影响，设全局即可）
    {
        std::locale utf8loc(std::locale::classic(), new std::codecvt_utf8_utf16<wchar_t>);
        std::locale::global(utf8loc);
    }
#endif
    
    // Initialize project root (--project <dir> or auto-detect from exe)
    ProjectManager::GetInstance().Initialize(argc, argv);
    const char* buildType =
#ifdef NDEBUG
        "Release";
#else
        "Debug";
#endif
    LOGI("==== MikanEngine starting (build %s) ====", buildType);

    // 解析体素世界开关：--no-voxel-world 关闭世界（纯 UI/2D 模式，体素世界整体不创建/更新/渲染）
    // --no-project-manager:跳过项目管理器启动页,直接以引擎根为项目进入(原型开发快捷方式;项目管理器代码保留)
    // --scene <path>: 加载指定场景文件(assets 相对路径,如 assets/snake.json),替代默认场景
    // --game <name>: 激活指定游戏模块(如 --game snake),场景加载后经 GameManager 调用
    // --no-editor: 强制纯游戏模式,即使 Editor.dll 存在也不加载(发布/性能测试形态)
    bool skipProjectManager = false;
    bool forceGameMode = false;
    bool headless = false;
    bool headlessNoRender = false;
    bool prefabSelftest = false;
    bool physics2dSelftest = false;
    int headlessFrames = 0;
    float fixedDeltaSeconds = 0.0f;
    bool invalidHeadlessArgs = false;
    std::string dumpStatePath;
    std::string dumpSchemaPath;
    std::string sceneArg;
    std::string gameArg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i] ? argv[i] : "";
        if (a == "--no-voxel-world") {
            g_EnableVoxelWorld = false;
        }
        if (a == "--zprepass") {
            // 2026-08-17：z-prepass 显式开启（默认关闭；大量三角形压力测试 CLI 对比用）
            g_EnableZPrepass = true;
        }
        if (a == "--no-project-manager") {
            skipProjectManager = true;
        }
        if (a == "--no-editor") {
            forceGameMode = true;
        }
        if (a == "--headless") {
            headless = true;
        }
        if (a == "--headless-no-render") {
            headless = true;
            headlessNoRender = true;
        }
        if (a == "--log-debug") {
            Core::SetLogLevel(Core::LogLevel::Debug);   // 打开 LOGD（编辑器/游戏详细日志）
        }
        if (a == "--frames" && i + 1 < argc) {
            const char* value = argv[i + 1] ? argv[i + 1] : "";
            char* end = nullptr;
            const long parsed = strtol(value, &end, 10);
            if (!end || *end != '\0' || parsed < 1 || parsed > 1000000L) {
                fprintf(stderr, "[Headless] ERROR: --frames must be an integer in [1, 1000000], got '%s'\n", value);
                invalidHeadlessArgs = true;
            } else {
                headlessFrames = static_cast<int>(parsed);
            }
            i++;
        }
        if (a == "--fixed-dt" && i + 1 < argc) {
            const char* value = argv[i + 1] ? argv[i + 1] : "";
            char* end = nullptr;
            const float parsed = strtof(value, &end);
            if (!end || *end != '\0' || parsed <= 0.0f || parsed > 0.1f) {
                fprintf(stderr, "[Headless] ERROR: --fixed-dt must be in (0, 0.1], got '%s'\n", value);
                invalidHeadlessArgs = true;
            } else {
                fixedDeltaSeconds = parsed;
            }
            i++;
        }
        if (a.rfind("--dump-state=", 0) == 0) {
            dumpStatePath = a.substr(13);
        } else if (a == "--dump-state" && i + 1 < argc) {
            dumpStatePath = argv[i + 1] ? argv[i + 1] : "";
            i++;
        }
        if (a.rfind("--dump-schema=", 0) == 0) {
            dumpSchemaPath = a.substr(14);
        } else if (a == "--dump-schema" && i + 1 < argc) {
            dumpSchemaPath = argv[i + 1] ? argv[i + 1] : "";
            i++;
        }
        if (a == "--prefab-selftest") {
            prefabSelftest = true;
        }
        if (a == "--phys2d-selftest") {
            physics2dSelftest = true;
        }
        if (a == "--cpu-skinning") {
            g_UseGpuSkinning = false;
            printf("GPU skinning disabled -> CPU skinning fallback\n");
        }
        if (a.rfind("--scene=", 0) == 0) {
            sceneArg = a.substr(8);
        } else if (a == "--scene" && i + 1 < argc) {
            sceneArg = argv[i + 1] ? argv[i + 1] : "";
            i++;
        }
        if (a.rfind("--game=", 0) == 0) {
            gameArg = a.substr(7);
        } else if (a == "--game" && i + 1 < argc) {
            gameArg = argv[i + 1] ? argv[i + 1] : "";
            i++;
        }
    }
    if (invalidHeadlessArgs) return 64;
    if (headless && fixedDeltaSeconds <= 0.0f) {
        fixedDeltaSeconds = 1.0f / 60.0f;
    }
#ifdef __ANDROID__
    // Android 几何直通模式：体素世界（WorldRenderer 管线创建在 Adreno 上崩）整体关闭，先保证应用启动
    g_EnableVoxelWorld = false;
    printf("Voxel world disabled on Android (WorldRenderer Adreno pipeline crash workaround)\n");
#endif
    if (!g_EnableVoxelWorld) {
        printf("Voxel world disabled (--no-voxel-world)\n");
    }
    if (skipProjectManager) {
        printf("Project manager skipped (--no-project-manager)\n");
    }
    if (headless) {
        forceGameMode = true;  // headless 不加载编辑器，走纯游戏渲染路径
        printf("Headless mode enabled (frames=%d, fixed_dt=%.6f, render=%s)\n",
            headlessFrames, fixedDeltaSeconds, headlessNoRender ? "off" : "on");
    }
    // 纯游戏模式(--no-editor)/headless: 编辑器未加载,项目管理器启动页(g_ProjectSelectionPending)无人渲染,
    // 等待选择会导致游戏永不运行。视为默认运行: 强制跳过项目管理器,直接以引擎根为项目进入,
    // 加载 --scene 指定场景或默认场景(场景顶层 "game" 键/--game 激活游戏模块)。
    if (forceGameMode) {
        skipProjectManager = true;
        printf("Game mode: auto-skipping project manager (standalone run)\n");
    }

    // --dump-schema <path>: 导出组件 schema JSON 后立即退出（不初始化窗口/Vulkan）
    if (!dumpSchemaPath.empty()) {
        ECS::RegisterAllComponentMeta();
        DumpSchema(dumpSchemaPath);
        return 0;
    }
    if (headless && headlessFrames <= 0 && !prefabSelftest && !physics2dSelftest) {
        fprintf(stderr, "[Headless] ERROR: --headless requires --frames N (N >= 1) unless a self-test is selected\n");
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

    // 初始化 SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }
    
    printf("SDL initialized successfully\n");

    // SDL3_image 3.0 不需要手动初始化

    // 获取显示缩放比例
    float main_scale = 1.0f;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (display != 0)
    {
        main_scale = SDL_GetDisplayContentScale(display) * 0.7f;
    }
    
    // 创建窗口
    SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    #ifndef __ANDROID__
    window_flags |= SDL_WINDOW_RESIZABLE;
    #endif
    if (headless) window_flags |= SDL_WINDOW_HIDDEN;  // headless: 不显示窗口
    window = SDL_CreateWindow(EngineConfig::WINDOW_TITLE, EngineConfig::WINDOW_WIDTH, EngineConfig::WINDOW_HEIGHT, window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return 1;
    }
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
        printf("Failed to create Vulkan surface.\n");
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
        printf("Editor disabled (--no-editor), forcing game mode\n");
    }
#endif
    if (editorActive)
        printf("Editor.dll loaded - editor mode\n");
    else {
        // Standalone game build: no editor -> force game mode + auto-enter registered project.
        // Must set skipProjectManager here (the forceGameMode check at startup already ran):
        // otherwise g_ProjectSelectionPending stays true with nobody rendering it -> black screen.
        g_RunMode = RunMode::Game;
        forceGameMode = true;
        skipProjectManager = true;
        printf("Editor.dll not found - running without editor (game mode)\n");
    }
    
    // 初始化ECS场景管理器
    ECS::SceneECS::GetInstance().Init();
    printf("SceneECS initialized successfully\n");
    
    // 注册物理系统到 ECS
    auto& coordinator = ECS::Coordinator::GetInstance();
    g_PhysicsSystemPtr = coordinator.RegisterSystem<ECS::PhysicsSystem>();
    {
        ECS::Signature signature;
        signature.set(coordinator.GetComponentType<ECS::TransformComponent>());
        signature.set(coordinator.GetComponentType<ECS::RigidBodyComponent>());
        coordinator.SetSystemSignature<ECS::PhysicsSystem>(signature);
    }
    printf("PhysicsSystem registered to ECS\n");
    
    // 初始化物理系统
    g_PhysicsSystemPtr->SetPhysicsManager(&g_PhysicsManager);
    g_PhysicsSystemPtr->Initialize();
    printf("PhysicsSystem initialized successfully\n");
    
    // 初始化 2D 物理(Box2D)
    Physics2DSystem::GetInstance().Initialize();

    // ===== 2D 物理自测(--phys2d-selftest): 不渲染/不依赖 UI, 直接验证重力下落 =====
    if (forceSelftest) {
        printf("[SELFTEST] Starting 2D physics self-test...\n");
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
                fprintf(stderr, "[SELFTEST] frame=%d pos=(%.3f, %.3f) mass=%.2f\n",
                        i, p.x, p.y, b2Body_GetMass(body));
            }
        }
        b2DestroyBody(body);
        b2DestroyWorld(world);
        printf("[SELFTEST] Done. If pos.y increased over frames -> gravity works.\n");
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
        printf("WorldSystem registered to ECS\n");
    }
    
    // 初始化音频管理器
    if (!AudioManager::GetInstance().Initialize()) {
        printf("Warning: Audio manager initialization failed\n");
    }
    
    // 初始化离屏渲染目标
    g_SceneRenderTarget.Init(w, h, true); // 启用MRT
    g_GameRenderTarget.Init(w, h, true);  // 启用MRT
    
    // 编辑器模式：先 Attach（初始化 ImGui），再创建 ImGui 描述符集
    if (editorActive) {
#ifdef _WIN32
        AttachEditor(window, w, h, main_scale);
#endif
        g_SceneRenderTarget.CreateImGuiDescriptorSet();
        g_GameRenderTarget.CreateImGuiDescriptorSet();
    }
    
    // 初始化场景渲染器 (使用离屏渲染目标的RenderPass)
    g_SceneRenderer.Init(g_SceneRenderTarget.GetRenderPass());
    g_SkyboxRenderer.Init(g_SceneRenderTarget.GetRenderPass());


    // 初始化全屏四边形渲染器（游戏模式合成 subpass：几何 subpass 0 + 合成 subpass 1，input attachment 读 G-Buffer）
    InitCompositeResources();
    g_AtmosphereRenderer.Init(w, h);   // 2026-08-11：compute 版（AtmosphereLUT，内部生成 LUT）
    // 更新合成描述符：绑定真实天空 RT（必须在 AtmosphereRenderer 初始化之后）
    UpdateFullscreenQuadDescriptors();
    // 初始化 2D 渲染核心（离屏世界层 + 主窗口 UI 层；玩法随 GameView 显示, UI 叠加在主窗口）
    if (!Renderer2D::GetInstance().Init(g_GameRenderTarget.GetRenderPass(), wd->RenderPass,
                                        g_GameRenderTarget.GetDisplayUIRenderPass(), g_CompositeUIPass)) {
        printf("ERROR: Renderer2D init failed (shader missing?)\n");
    }
    printf("Renderer2D initialized\n");
    
    // 初始化文本渲染器（依赖 Renderer2D 的 descriptor layout）
    {
        std::string fontPath = EngineConfig::GetFontPath("simhei.ttf");
        if (TextRenderer::GetInstance().Init(fontPath, 24.0f)) {
            printf("TextRenderer initialized with %s\n", fontPath.c_str());
        } else {
            printf("WARNING: TextRenderer init failed (no font file?)\n");
        }
    }

    // 开屏 Logo 使用项目资源（Android 由 sync_assets.ps1 同步到 APK assets/ui）。
    // 加载失败时仍保留文字版加载页，不阻断原型启动。
    const std::string splashLogoPath = EngineConfig::GetFullPath("ui/mikan_engine_splash.png");
    if (Renderer2D::GetInstance().LoadTexture("mikan_engine_splash", splashLogoPath)) {
        printf("Loading splash logo ready: %s\n", splashLogoPath.c_str());
    } else {
        printf("WARNING: Loading splash logo unavailable: %s\n", splashLogoPath.c_str());
    }

    // 独立游戏启动时，场景加载发生在主循环之前；先提交一帧轻量加载页，
    // 让 Android 在模型/纹理/脚本准备期间始终有可见反馈。编辑器启动页不走这条路径。
    const bool showStartupLoading = forceGameMode && !editorActive && !headless;
    auto renderStartupLoading = [&](float progress, const char* status) {
        if (!showStartupLoading) return;
        SetLoadingScreenState(true, progress, status);
        const glm::mat4 identity(1.0f);
        ::FrameRender(wd, nullptr, identity, identity);
        ::FramePresent(wd);
    };

    auto renderStartupSplash = [&]() {
        if (!showStartupLoading) return;
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
    };

    renderStartupSplash();
    renderStartupLoading(0.05f, "Preparing renderer...");
    
    // 所有系统初始化完成后加载场景。
    // --project 已显式指定 → 直接加载项目场景；
    // --no-project-manager → 跳过项目管理器,直接以引擎根为项目进入(原型开发快捷方式)；
    // --scene <path> → 加载指定场景文件(如 assets/snake.json),优先于默认场景；
    // 否则 → 显示项目管理器启动页,选择项目后由 MikanEngine_OpenProject 加载。
    if (ProjectManager::GetInstance().IsExplicitProject() || skipProjectManager) {
        bool loaded = false;
        renderStartupLoading(0.12f, "Loading scene...");
        if (!sceneArg.empty()) {
            std::string scenePath = ProjectManager::GetInstance().ResolveAssetPath(sceneArg);
            ECS::SceneSerializer sceneLoader;
            if (sceneLoader.LoadScene(scenePath)) {
                printf("Scene loaded: %s\n", scenePath.c_str());
                loaded = true;
            } else {
                printf("Failed to load scene '%s', falling back to default\n", scenePath.c_str());
                if (headless) {
                    // headless: 场景加载失败是硬错误，绝不 fallback（否则 AI 会把默认场景误判为验证通过）
                    printf("[Headless] Scene load FAILED, aborting (exit=2)\n");
                    return 2;
                }
            }
        }
        if (!loaded) {
#ifdef __ANDROID__
            // Android 原型（2026-08-23）：无命令行参数机制，直接加载第三人称原型场景。
            // 场景顶层含 "game":"cesiumwalk"，对应玩法已静态编入 Android so。
            // SceneSerializer::LoadScene 的 Android 分支走 SDL_IOFromFile（APK assets 安全）。
            {
                ECS::SceneSerializer sceneLoader;
                if (sceneLoader.LoadScene("third_person_prototype.json")) {
                    printf("Android proto scene loaded: third_person_prototype.json\n");
                    LOGI("Android proto scene loaded: third_person_prototype.json");
                    loaded = true;
                } else {
                    printf("Android proto scene 'third_person_prototype.json' load failed, falling back to default\n");
                    LOGI("Android proto scene 'third_person_prototype.json' load FAILED, falling back to default");
                }
            }
            if (!loaded)
                ECS::SceneECS::GetInstance().LoadDefaultScene();
#else
            // 显式项目必须优先使用自己的 project.json.scene；不能被发布版的
            // projects.json 自动项目选择逻辑覆盖。旧式项目则继续走默认场景。
            bool registeredProjectOpened = false;
            if (ProjectManager::GetInstance().IsManifestProject()) {
                const ProjectManifest& manifest =
                    ProjectManager::GetInstance().GetManifest();
                if (!manifest.scene.empty()) {
                    const std::string manifestScenePath =
                        ProjectManager::GetInstance().ResolveAssetPath(manifest.scene);
                    ECS::SceneSerializer sceneLoader;
                    if (sceneLoader.LoadScene(manifestScenePath)) {
                        printf("Project manifest scene loaded: %s\n",
                               manifestScenePath.c_str());
                        loaded = true;
                    } else {
                        printf("Failed to load project manifest scene '%s', falling back to default\n",
                               manifestScenePath.c_str());
                        if (headless) {
                            printf("[Headless] Project manifest scene load FAILED, aborting (exit=2)\n");
                            return 2;
                        }
                    }
                }
            }

            // 未指定显式项目时，发布包优先打开 projects.json 的第一个项目。
            if (!loaded && !ProjectManager::GetInstance().IsExplicitProject()) {
                registeredProjectOpened = OpenFirstRegisteredProject();
            }
            if (!loaded && !registeredProjectOpened) {
                printf("Loading default scene after all systems initialized...\n");
                ECS::SceneECS::GetInstance().LoadDefaultScene();
                printf("Default scene loaded successfully\n");
            }
#endif
        }
        Physics2DSystem::GetInstance().ClearBodies(); // 场景重建后清理旧 2D 刚体
        Camera2DSystem::GetInstance().SetSceneContext(
            sceneArg.empty() ? "<project/default>" : ProjectManager::GetInstance().ResolveAssetPath(sceneArg));
        Camera2DSystem::GetInstance().Reset();
        Camera2DSystem::GetInstance().Rebind();
        ThirdPersonCameraSystem::GetInstance().SetSceneContext(
            sceneArg.empty() ? "<project/default>" : ProjectManager::GetInstance().ResolveAssetPath(sceneArg));
        ThirdPersonCameraSystem::GetInstance().Reset();

        // 瓦片地图: 遍历场景加载所有带 TilemapComponent 的实体(TMX/自产解析 + 图集纹理 + Box2D 碰撞体)
        TilemapSystem::GetInstance().LoadAllFromScene();
        renderStartupLoading(0.78f, "Preparing gameplay...");

        // 激活游戏模块: 优先 --game <name> 参数;否则按场景文件顶层 "game" 键自动激活
        // (项目管理器/导入场景文件打开时无需命令行参数,场景自带游戏标记)
        std::string effectiveGame = gameArg;
        if (effectiveGame.empty()) {
            effectiveGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
        }
        if (!effectiveGame.empty()) {
            if (auto* gm = Game::GameManager::GetInstance().Activate(effectiveGame)) {
                gm->OnSceneLoaded();
            }
            // 补齐脚本实例：插件 DLL 在 Activate 时才加载并注册脚本工厂，而场景反序列化
            // （InstantiateAll）可能早于它——此处幂等补齐缺失实例（已创建的不动）。
            ECS::ScriptSystem::GetInstance().InstantiateAll(false);

            // 预制体自测：保存 Baka 子树 -> 实例化 -> 断言（--prefab-selftest）
            if (prefabSelftest) {
                return RunPrefabSelftest();
            }
        }
        renderStartupLoading(0.94f, "Starting prototype...");
    } else {
        g_ProjectSelectionPending = true;
        printf("No explicit project: showing project manager (pending selection)\n");
    }

    // ===== 2D Canvas 渲染核心初始化（场景中的 2D 实体由场景文件 / 编辑器添加；
    // 2D 世界层相机由场景树的 Camera2DComponent 每帧驱动,无组件时默认 (0,0,1)）=====
    {
        auto& canvas = UI::Canvas2D::GetInstance();
        canvas.SetViewport(w, h);
    }
    if (showStartupLoading) {
        SetLoadingScreenState(false, 1.0f, "Ready");
    }

    // 主循环
    bool done = false;
    int frameCount = 0;
    while (!done)
    {
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - g_LastTime).count();
        g_LastTime = currentTime;
        if (headless) deltaTime = fixedDeltaSeconds;
        // 限制单帧最大步长（100ms = ~10fps 下限）：
        // 调试断点、窗口最小化、驱动卡顿都会产生大 deltaTime，直接传给 Jolt 可能积分出爆炸的力/位移。
        // 超过上限时丢帧（逻辑少推进而不是一次性补巨大步长），保持物理稳定。
        if (deltaTime > 0.1f) deltaTime = 0.1f;

        // 平滑帧率(游戏画面 FPS 显示用)
        g_FPS = g_FPS * 0.9f + (1.0f / (deltaTime > 0.0001f ? deltaTime : 0.0001f)) * 0.1f;

#ifdef __ANDROID__
        // 低频运行时诊断：用于真机确认主循环、帧率和 native 内存状态。
        // 不记录每帧，避免 logcat 刷屏影响性能和诊断结果。
        if ((frameCount % 120) == 0 && frameCount > 0) {
            long residentKb = 0;
            std::ifstream statusFile("/proc/self/status");
            std::string statusLine;
            while (std::getline(statusFile, statusLine)) {
                if (statusLine.rfind("VmRSS:", 0) == 0) {
                    std::sscanf(statusLine.c_str(), "VmRSS: %ld kB", &residentKb);
                    break;
                }
            }
            LOGI("[Runtime] frame=%d fps=%.1f dt=%.2fms native_rss=%ldKB",
                 frameCount, g_FPS, deltaTime * 1000.0f, residentKb);
        }
#endif

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            // 调试：打印所有事件类型
            #ifdef __ANDROID__
            if (event.type >= SDL_EVENT_FIRST && event.type <= SDL_EVENT_LAST) {
                LOGD("[SDL Event] Type: %u, Timestamp: %llu", event.type, event.common.timestamp);
                if (event.type == SDL_EVENT_WINDOW_RESIZED || 
                    event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                    event.type == SDL_EVENT_WINDOW_SHOWN ||
                    event.type == SDL_EVENT_WINDOW_HIDDEN ||
                    event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                    event.type == SDL_EVENT_WINDOW_RESTORED) {
                    LOGI("[SDL Window Event] WindowID: %u, Data1: %d, Data2: %d", 
                           event.window.windowID, event.window.data1, event.window.data2);
                }
            }
            #endif
            
            // Dear ImGui 1.92 保留了基础 Tab 遍历，即使关闭 NavEnableKeyboard 也会
            // 产生焦点蓝框。只过滤发给 ImGui 的 Tab，下面的引擎/游戏输入仍能收到它。
            const bool isTabEvent =
                (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                event.key.key == SDLK_TAB;
            if (editorActive && !isTabEvent)
                ImGui_ImplSDL3_ProcessEvent(&event);
            // 设置页打开时优先消费鼠标/触摸，避免点击选项同时被解释成移动、视角或动作。
            const bool settingsConsumed =
                (g_RunMode == RunMode::Game) &&
                UI::RuntimeSettingsOverlay::GetInstance().ProcessEvent(event);
            if (!settingsConsumed)
                g_InputController.ProcessInput(event, g_Camera, deltaTime);
            // 键盘事件转发给当前游戏模块(引擎不感知具体游戏)
            if (!settingsConsumed && event.type == SDL_EVENT_KEY_DOWN) {
                // F 键: 切换游戏画面 FPS 显示(全局,菜单提示中有说明)
                if (event.key.key == SDLK_F) {
                    g_ShowFPS = !g_ShowFPS;
                    printf("FPS display %s\n", g_ShowFPS ? "on" : "off");
                }
                // T 键: 切换 2D 碰撞体线框调试显示(物理排错)
                if (event.key.key == SDLK_T) {
                    g_ShowPhysics2DDebug = !g_ShowPhysics2DDebug;
                    printf("Physics2D debug %s\n", g_ShowPhysics2DDebug ? "on" : "off");
                }
                if (auto* gm = Game::GameManager::GetInstance().GetCurrent()) {
                    gm->OnKey(event.key.key);
                }
            }
            if (event.type == SDL_EVENT_QUIT)
            {
                done = true;
                g_IsPaused = true; // 立即暂停渲染
                break; // 立即跳出事件循环
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
            {
                done = true;
                g_IsPaused = true; // 立即暂停渲染
                break; // 立即跳出事件循环
            }
            #ifndef __ANDROID__
            if (event.type == SDL_EVENT_WINDOW_RESIZED && event.window.windowID == SDL_GetWindowID(window))
            {
                g_SwapChainRebuild = true;
            }
            #endif
            #ifdef __ANDROID__
            // 处理 Android 特有的事件
            if (event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_WINDOW_HIDDEN)
            {
                g_IsPaused = true; // 窗口最小化或隐藏时暂停渲染
                LOGI("[Android] Window minimized/hidden - pausing render");
            }
            if (event.type == SDL_EVENT_WINDOW_SHOWN || event.type == SDL_EVENT_WINDOW_RESTORED)
            {
                g_IsPaused = false; // 窗口显示或恢复时恢复渲染
                LOGI("[Android] Window shown/restored - resuming render");
                
                // Android 从后台恢复时，即使没有 RESIZED 事件，也需要重建 surface
                // 因为 surface 可能已经被系统销毁并重新创建
                g_SwapChainRebuild = true;
                LOGI("[Android] Marking swapchain for rebuild on resume");
            }
            // Android 上 Surface 重建时也需要重建 swapchain
            if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                g_SwapChainRebuild = true;
                LOGI("[Android] Surface changed - rebuilding swapchain (size: %dx%d)", event.window.data1, event.window.data2);
                
                // Surface 改变后立即更新 SDL 对应的 Vulkan surface；同时由
                // CreateAndroidVulkanSurface 保护 Java SurfaceView 的短暂空窗。
                if (g_MainWindowData.Surface != VK_NULL_HANDLE) {
                    LOGI("[Android] Destroying old Vulkan surface");
                    vkDestroySurfaceKHR(g_Instance, g_MainWindowData.Surface, g_Allocator);
                    g_MainWindowData.Surface = VK_NULL_HANDLE;
                }
                
                LOGI("[Android] Creating new Vulkan surface");
                VkSurfaceKHR newSurface;
                if (!CreateAndroidVulkanSurface(window, g_Instance, g_Allocator, &newSurface)) {
                    LOGE("[Android] Failed to create new Vulkan surface!");
                } else {
                    g_MainWindowData.Surface = newSurface;
                    LOGI("[Android] New Vulkan surface created successfully");
                }
            }
            #endif
        }

        // 检查是否需要退出
        if (done)
        {
            break; // 立即跳出主循环
        }

        // 处理游戏模式/编辑器模式切换
        g_InputController.ProcessModeToggle();
        
        if (g_RunMode == RunMode::Game) {
            g_InputController.UpdateSceneCamera(deltaTime);
        } else {
            g_InputController.Update(g_Camera, deltaTime);
            g_Camera.Update(deltaTime);
        }
        
        // 更新物理系统
        bool gameRunning = true;
        bool gamePaused = false;
#ifdef _WIN32
        if (editorActive && s_editorIsGameRunning) gameRunning = s_editorIsGameRunning();
        if (editorActive && s_editorIsGamePaused) gamePaused = s_editorIsGamePaused();
#endif

        // 播放/暂停/停止状态接入游戏模块: 检测状态边沿, 区分"停止"与"暂停"
        {
            // s_wasGameRunning 跟踪工具栏的原始运行位，而不是
            // gameRunning && !gamePaused。否则从暂停状态点击停止时，
            // 上一帧会被误记为 false，停止回调和场景恢复都会被跳过。
            static bool s_wasGameRunning = false;
            static bool s_wasPaused = false;
            auto* gm = Game::GameManager::GetInstance().GetCurrent();
            if (!s_wasGameRunning && gameRunning) {
                if (editorActive) {
                    CaptureEditorPlaySnapshot();
                }
                if (gm) gm->OnGameStart();            // 播放: 进入运行态
            } else if (s_wasGameRunning && !gameRunning) {
                if (gm) gm->OnGameStop();             // 停止: 先通知游戏清理运行时状态
                if (editorActive) {
                    RestoreEditorPlaySnapshot();      // 停止: 恢复播放前场景
                    s_editorPlaySnapshot.clear();
                    s_editorPlaySnapshotValid = false;
                }
            } else if (s_wasGameRunning && !s_wasPaused && gamePaused) {
                if (gm) gm->OnGamePause();            // 暂停: 只冻结时间,不重置
            } else if (s_wasGameRunning && s_wasPaused && !gamePaused) {
                if (gm) gm->OnGameResume();           // 恢复
            }
            s_wasGameRunning = gameRunning;
            s_wasPaused = gamePaused;
        }
        if (!g_IsPaused && gameRunning && !gamePaused) {
            // 玩家控制器只写入动态刚体速度/朝向，再由下面的 Jolt Step
            // 统一处理地形接触和 Transform 回写。
            coordinator.GetSystem<ECS::PlayerControllerSystem>()->Update(deltaTime);
            // 获取相机位置用于清理远距离刚体
            glm::vec3 cameraPos = g_Camera.Position;
            g_PhysicsSystemPtr->Update(deltaTime, cameraPos);
            // 2D 物理(Box2D): 仅运行态(工具栏播放)步进, 暂停/停止冻结
            Physics2DSystem::GetInstance().Update(deltaTime);
        }
        
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }
        
        // Android 平台：每次渲染前检查 surface 是否有效
        #ifdef __ANDROID__
        if (g_MainWindowData.Surface == VK_NULL_HANDLE || g_MainWindowData.Swapchain == VK_NULL_HANDLE) {
            LOGI("[Android] Surface or Swapchain is null, rebuilding...");
            g_SwapChainRebuild = true;
        }
        #endif

        if (g_SwapChainRebuild)
        {
            int new_width, new_height;
            SDL_GetWindowSize(window, &new_width, &new_height);
            
            // 如果尺寸为 0，跳过重建
            if (new_width == 0 || new_height == 0)
            {
                LOGI("[Android] Window size is 0, skipping rebuild");
                g_SwapChainRebuild = false;
                SDL_Delay(10);
                continue;
            }

            LOGI("[Android] Starting swapchain rebuild process...");
            
            // Android 平台：总是先销毁并重新创建 surface
            // 因为从后台恢复时，即使 surface 句柄有效，底层 surface 也可能已经被系统销毁
            #ifdef __ANDROID__
            if (g_MainWindowData.Surface != VK_NULL_HANDLE) {
                LOGI("[Android] Destroying old Vulkan surface before rebuild");
                vkDestroySurfaceKHR(g_Instance, g_MainWindowData.Surface, g_Allocator);
                g_MainWindowData.Surface = VK_NULL_HANDLE;
            }
            
            // 创建新的 surface
            LOGI("[Android] Creating new Vulkan surface");
            VkSurfaceKHR newSurface;
            if (!CreateAndroidVulkanSurface(window, g_Instance, g_Allocator, &newSurface)) {
                LOGE("[Android] Failed to create Vulkan surface!");
                SDL_Delay(100);
                continue;
            }
            g_MainWindowData.Surface = newSurface;
            LOGI("[Android] New surface created successfully, handle: %p", (void*)newSurface);
            g_IsPaused = false;
            #else
            // 其他平台：仅在 surface 为空时创建
            if (g_MainWindowData.Surface == VK_NULL_HANDLE) {
                LOGI("[Platform] Surface is null, creating new surface...");
                VkSurfaceKHR newSurface;
                if (!SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &newSurface)) {
                    LOGE("[Platform] Failed to create Vulkan surface!");
                    g_SwapChainRebuild = false;
                    SDL_Delay(100);
                    continue;
                }
                g_MainWindowData.Surface = newSurface;
                LOGI("[Platform] New surface created successfully");
            }
            #endif
            
            // RecreateSwapChain 已经处理了渲染目标的重建和描述符集更新
            ::RecreateSwapChain(new_width, new_height);
            
            g_SwapChainRebuild = false;
            LOGI("[Android] Swapchain rebuild completed");
        }

        // 编辑器帧：全部 UI（ImGui/窗口/Gizmo）由 Editor.dll 提供
        ImDrawData* draw_data = nullptr;
#ifdef _WIN32
        if (editorActive && s_editorRenderFrame) {
            // 每帧同步渲染目标描述符（resize 重建后 descriptor 会变化）
            // 用"显示附件"（合成 subpass 输出）——SceneView/GameView 面板显示 fullscreen.frag 后处理结果
            if (s_editorSetSceneViewDesc) s_editorSetSceneViewDesc(g_SceneRenderTarget.GetDisplayDescriptorSet());
            if (s_editorSetGameViewDesc) s_editorSetGameViewDesc(g_GameRenderTarget.GetDisplayDescriptorSet());
            s_editorRenderFrame();
        }
        if (editorActive) {
            draw_data = ImGui::GetDrawData();
        }
#endif

        // 只有编辑器模式依赖 draw_data 判断最小化；独立游戏模式始终渲染
        const bool is_minimized = (editorActive && (!draw_data || draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f));
        if (!is_minimized && !g_IsPaused)
        {
            glm::mat4 view = g_Camera.GetViewMatrix();
            glm::mat4 proj = glm::perspective(glm::radians(EngineConfig::FOV), (float)wd->Width / (float)wd->Height, EngineConfig::NEAR_PLANE, EngineConfig::FAR_PLANE);
            proj[1][1] *= -1;

            ImVec4 clear_color = ImVec4(0.1f, 0.1f, 0.1f, 1.00f);
            wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            wd->ClearValue.color.float32[3] = clear_color.w;
            
            // Android 平台：渲染前检查 surface 状态
            #ifdef __ANDROID__
            if (wd->Surface == VK_NULL_HANDLE || wd->Swapchain == VK_NULL_HANDLE) {
                LOGI("[Android] Render: Surface/Swapchain invalid, triggering rebuild");
                g_SwapChainRebuild = true;
                continue; // 跳过本次渲染，下一帧会重建
            }
            #endif
            
            // 更新体素世界（chunk 生成/网格重建，从 OpenGL 版迁移）
            if (g_EnableVoxelWorld && g_WorldSystemPtr) {
                // 世界生成中心 + 视锥：优先游戏主相机（与 model 渲染的剔除基准一致），fallback 到编辑器相机
                glm::vec3 worldCamPos = g_Camera.Position;
                glm::mat4 worldView = view, worldProj = proj;
                {
                    glm::mat4 wv, wp;
                    glm::vec3 wcp;
                    float aspect = (float)g_GameRenderTarget.GetWidth() / (float)g_GameRenderTarget.GetHeight();
                    if (g_SceneRenderer.GetMainCameraMatrices(aspect, wv, wp, wcp)) {
                        worldCamPos = wcp;
                        worldView = wv;
                        worldProj = wp;
                    }
                }
                auto worldFrustum = AABBUtils::ExtractFrustumPlanes(worldProj * worldView, -10.0f);
                g_WorldSystemPtr->Update(deltaTime, worldCamPos, worldFrustum);

                // 方块交互（从 OpenGL 版迁移）：游戏模式下左键破坏 / 右键放置
                if (g_RunMode == RunMode::Game && g_World != nullptr) {
                    // 活动相机（游戏模式优先场景主相机）
                    glm::vec3 camPos = g_Camera.Position;
                    glm::vec3 camFront = g_Camera.Front;
                    {
                        glm::mat4 wv, wp;
                        glm::vec3 wcp;
                        float aspect = (float)g_GameRenderTarget.GetWidth() / (float)g_GameRenderTarget.GetHeight();
                        if (g_SceneRenderer.GetMainCameraMatrices(aspect, wv, wp, wcp)) {
                            camPos = wcp;
                            camFront = -glm::vec3(wv[0][2], wv[1][2], wv[2][2]);
                        }
                    }
                    World::HitResult hit = g_World->RayCast(camPos, camFront, 10.0f);
                    const int handBlockId = g_HandBlockId; // 手持方块类型（物品栏选中格写入；默认 3=草方块）
                    g_InputController.HandleBlockInteraction(*g_World, handBlockId, hit, deltaTime);
                }
            }
            
            // ===== 2D Canvas 交互更新（无按钮时仅命中测试，无副作用）=====
            {
                // 引擎标准输入: 快照本帧键盘/鼠标状态(游戏经 InputSystem 查询动作)
                Input::InputSystem::GetInstance().Update();
                // Cinemachine 风格 2D 智能相机: 计算 Camera2DComponent.center(跟随/阻尼/前视/边界/震动)
                Camera2DSystem::GetInstance().SetViewport((uint32_t)w, (uint32_t)h);
                Camera2DSystem::GetInstance().Update(deltaTime);
                // 2D 世界层相机:场景树 Camera2DComponent 驱动(无组件默认)
                UI::Canvas2D::GetInstance().SyncCameraFromScene();
                float mx = 0.0f, my = 0.0f;
                Uint32 mouseState = SDL_GetMouseState(&mx, &my);
                UI::Canvas2D::GetInstance().Update(glm::vec2(mx, my),
                    (mouseState & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0);
                UI::TweenSystem::GetInstance().Update(deltaTime);
                // 2D 精灵帧动画(序2): 推进帧号写回 Sprite2D.uv0/uv1
                SpriteAnimatorSystem::GetInstance().Update(deltaTime);
                // 音频源组件驱动（场景树 AudioSourceComponent → AudioManager）
                ECS::AudioSourceSystem::GetInstance().Update(deltaTime);
                // 游戏模块: 始终执行回调(开始后即需响应的逻辑, 如玩家控制/相机)
                if (auto* gm = Game::GameManager::GetInstance().GetCurrent()) {
                    gm->OnAlwaysUpdate(deltaTime);
                }
                // 游戏模块: 逻辑更新仅在运行态(播放且未暂停)时调用
                if (gameRunning && !gamePaused) {
                    // 骨骼动画：推进所有模型动画 + 更新蒙皮矩阵（先于渲染）
                    g_SceneRenderer.UpdateModelAnimations(deltaTime);
                    // 脚本组件（Unity 式玩法挂载）先于游戏模块更新
                    ECS::ScriptSystem::GetInstance().Update(deltaTime);
                    if (auto* gm = Game::GameManager::GetInstance().GetCurrent()) {
                        gm->OnUpdate(deltaTime);
                    }
                    // 粒子属于游戏逻辑阶段：每帧只模拟一次，多个视口渲染只读取
                    // 同一份实例快照，避免编辑器 SceneView/GameView 各自推进生命周期。
                    ParticleSystem::GetInstance().Update(deltaTime);
                }
            }

            // 基础 3D 第三人称轨道相机: 保持原有的 UI/脚本更新时序，
            // 在玩法更新后写回主相机 Transform。玩家脚本读取的是上一帧已
            // 完成的相机姿态，但方向仍然完全来自相机前向，不使用世界固定轴。
            // 编辑器播放时 g_RunMode 仍可能保持 Editor，但 gameRunning 已经表示真正的播放态；
            // 这里必须按实际运行态更新，否则 GameView 中鼠标/滚轮输入永远不会到达第三人称系统。
#ifndef __ANDROID__
            const bool gameplayRunning =
                (g_RunMode == RunMode::Game && !gamePaused) ||
                (editorActive && s_editorIsGameRunning && gameRunning && !gamePaused);
#else
            const bool gameplayRunning = (g_RunMode == RunMode::Game && !gamePaused);
#endif
            if (gameplayRunning) {
                ThirdPersonCameraSystem::GetInstance().Update(deltaTime);
            }

            if (!headlessNoRender) {
                ::FrameRender(wd, draw_data, view, proj);
                ::FramePresent(wd);
            }

            // 所有运行模式都维护帧计数；headless 还用它判断自动退出。
            frameCount++;

            // ===== headless: 固定逻辑帧数后自动退出 =====
            if (headless) {
                if (headlessFrames > 0 && frameCount >= headlessFrames) {
                    printf("[Headless] Reached frame limit (%d), exiting\n", headlessFrames);
                    done = true;
                }
            }
        }
    }

    // ===== headless: 导出场景状态（在清理之前，保留最终状态）=====
    int finalExitCode = 0;
    if (!dumpStatePath.empty() && !DumpSceneState(dumpStatePath, frameCount)) finalExitCode = 3;

    // 清理资源
    err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);
    
    // 卸载编辑器（Editor.dll 内部清理 ImGui）
#ifdef _WIN32
    ShutdownEditorDll();
#endif
    
    // 清理 2D Canvas（释放节点树）
    UI::Canvas2D::GetInstance().Clear();

    // 清理物理系统
    g_PhysicsSystemPtr->Shutdown();
    
    // 清理体素世界系统（在渲染器清理之前，确保 World 数据不再被访问）
    if (g_EnableVoxelWorld && g_WorldSystemPtr) {
        g_WorldSystemPtr->Shutdown();
    }
    

    // 清理渲染器（必须在 CleanupVulkan 之前）
    std::cout << "[EngineMain] Cleaning renderers..." << std::endl;
    g_SceneRenderer.Cleanup();
    std::cout << "[EngineMain] SceneRenderer cleanup done" << std::endl;
    
    g_SkyboxRenderer.Cleanup();
    std::cout << "[EngineMain] SkyboxRenderer cleanup done" << std::endl;
    
    g_SceneRenderTarget.Cleanup();
    std::cout << "[EngineMain] SceneRenderTarget cleanup done" << std::endl;
    
    g_GameRenderTarget.Cleanup();
    std::cout << "[EngineMain] GameRenderTarget cleanup done" << std::endl;
    
    g_FullscreenQuad.Cleanup();
    std::cout << "[EngineMain] FullscreenQuad cleanup done" << std::endl;

    g_SceneCompositeQuad.Cleanup();
    g_GameCompositeQuad.Cleanup();
    g_SceneFilterQuad.Cleanup();
    g_GameFilterQuad.Cleanup();
    g_SceneChain.Cleanup();
    g_GameChain.Cleanup();
    g_SwapChain.Cleanup();
    std::cout << "[EngineMain] CompositeQuad cleanup done" << std::endl;

    g_AtmosphereRenderer.Cleanup();
    std::cout << "[EngineMain] AtmosphereRenderer cleanup done" << std::endl;
    
    // 清理文本渲染器和字体图集（必须在 Vulkan 设备销毁前）
    TextRenderer::GetInstance().Cleanup();
    Renderer2D::GetInstance().Cleanup();
    std::cout << "[EngineMain] TextRenderer/Renderer2D cleanup done" << std::endl;
    
    ::CleanupVulkanWindow();
    ::CleanupVulkan();
    std::cout << "[EngineMain] Vulkan cleanup done" << std::endl;
    
    // 清理音频管理器
    std::cout << "[EngineMain] Destroying audio manager..." << std::endl;
    AudioManager::DestroyInstance();
    std::cout << "[EngineMain] Audio manager destroyed" << std::endl;
    
    // 清理物理系统（在 Vulkan 设备销毁之后，全局对象析构之前）
    CleanupPhysicsSystem();
    
    std::cout << "[EngineMain] Destroying window..." << std::endl;
    SDL_DestroyWindow(window);
    std::cout << "[EngineMain] Window destroyed" << std::endl;
    
    std::cout << "[EngineMain] Quitting SDL..." << std::endl;
    SDL_Quit();
    std::cout << "[EngineMain] SDL quit" << std::endl;
    
    std::cout << "[EngineMain] Exit successfully" << std::endl;
    std::cout << "[EngineMain] Returning from main()" << std::endl;
    LOGI("==== MikanEngine shutting down cleanly ====");
    Core::ShutdownLog();
    return finalExitCode;
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
        printf("SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
    }
}
#endif
