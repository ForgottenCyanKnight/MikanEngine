#include "SceneRenderer.h"
#include "EngineGlobal.h"
#include "SkyboxRenderer.h"
#include "RenderTarget.h"
#include "FullscreenQuad.h"
#include "AtmosphereRenderer.h"
#include "EngineConfig.h"
#include "PhysicsManager.h"
#include "Core/SceneManager.h"
#include "ECS/PhysicsSystem.h"
#include "World/WorldGlobals.h"
#include "World/WorldRenderer.h"
#include "ECS/Systems/WorldSystem.h"
#include "ECS/SceneECS.h"
#include "Core/ProjectManager.h"
#include "Game/GameManager.h"
#include "SceneSerializer.h"
#include "Core/Physics2DSystem.h"
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <SDL3/SDL.h>
#include <iostream>


SceneRenderer g_SceneRenderer;
SkyboxRenderer g_SkyboxRenderer;
RenderTarget g_SceneRenderTarget;
RenderTarget g_GameRenderTarget;
FullscreenQuad g_FullscreenQuad;
FullscreenQuad g_SceneCompositeQuad;      // 编辑器 SceneView 合成（SceneRT render pass subpass1 → 显示附件）
FullscreenQuad g_GameCompositeQuad;       // 编辑器 GameView 合成（GameRT render pass subpass1 → 显示附件）
FullscreenQuad g_SceneFilterQuad;         // 编辑器 SceneView final 后处理（黑白滤镜测试）
FullscreenQuad g_GameFilterQuad;          // 编辑器 GameView final 后处理

#include "Rendering/PostProcessChain.h"
PostProcessChain g_SceneChain;             // 编辑器 SceneView 后处理链（配置驱动）
PostProcessChain g_GameChain;              // 编辑器 GameView 后处理链
PostProcessChain g_SwapChain;              // 游戏模式 swapchain 后处理链
AtmosphereRenderer g_AtmosphereRenderer;   // physical sky (low-res sky RT)

// physical sky toggle (default on: 3D scenes with SkyboxComponent use atmosphere instead of cubemap)
bool g_AtmosphereEnabled = true;

// 浣撶礌涓栫晫鍏ㄥ眬鎸囬拡锛堢敱 WorldSystem 绠＄悊锛涙覆鏌?纰版挒閫氳繃瀹冭闂級
World* g_World = nullptr;
WorldRenderer* g_WorldRenderer = nullptr;

// 浣撶礌涓栫晫杩愯鏃跺紑鍏筹紙榛樿寮€鍚紱鍛戒护琛?--no-voxel-world 鍏抽棴锛岀敤浜庣函 UI/2D 妯″紡锛? (encoding-repaired)
bool g_EnableVoxelWorld = true;
int g_HandBlockId = 3;  // 手持方块类型（物品栏选中格写入；EngineMain 放置用）

// 浣撶礌涓栫晫 ECS 绯荤粺
std::shared_ptr<ECS::WorldSystem> g_WorldSystemPtr = nullptr;

// 娣诲姞涓€涓緟鍔╃被鏉ヨ拷韪叏灞€瀵硅薄鏋愭瀯
struct GlobalDestructorTracker {
    ~GlobalDestructorTracker() {
        std::cout << "[GlobalDestructorTracker] All global objects destroyed" << std::endl;
    }
};
static GlobalDestructorTracker g_GlobalDestructorTracker;

// 鍏ㄥ眬绐楀彛鍙橀噺
SDL_Window* window = nullptr;

// 鏃堕棿鐩稿叧
auto g_LastTime = std::chrono::high_resolution_clock::now();

extern bool g_IsPaused;

// 为 ECS 命名空间提供 g_SceneRenderer
namespace ECS {
    // 定义 g_SceneRenderer 为全局变量的引用
    SceneRenderer& g_SceneRenderer = ::g_SceneRenderer;
}

// 鐗╃悊绯荤粺鍏ㄥ眬鍙橀噺
Physics::PhysicsManager g_PhysicsManager;
std::shared_ptr<ECS::PhysicsSystem> g_PhysicsSystemPtr = nullptr;

// 鍦?main() 涓墜鍔ㄦ竻鐞?PhysicsSystem锛岄伩鍏嶅湪鏋愭瀯鏃惰闂凡閿€姣佺殑璧勬簮
void CleanupPhysicsSystem() {
    std::cout << "[EngineGlobals] Cleaning up physics system..." << std::endl;
    g_PhysicsSystemPtr.reset();
    std::cout << "[EngineGlobals] Physics system cleanup completed" << std::endl;
}

// 鐩告満纰版挒
bool g_CameraCollisionEnabled = false;

// DLL module globals (declared in Core/EngineGlobal.h)
TexturePool* g_TexturePool = nullptr;
bool g_ShowSceneView = false;
bool g_ShowGameView = false;
bool g_ShowGrid = true;
bool g_ProjectSelectionPending = false;  // 鍚姩鏈寚瀹?--project:绛夊緟椤圭洰绠＄悊鍣ㄩ€夋嫨椤圭洰
bool g_SceneIs2D = false;
bool g_EnableZPrepass = false;   // z-prepass 默认关闭（2026-08-17：大量三角下 2× 顶点处理可能负收益，GUI 直接测默认态；CLI --zprepass 开启对比）
bool g_ShowFPS = true;                   // 娓告垙鐢婚潰 FPS 鏄剧ず寮€鍏?鑿滃崟 F 閿垏鎹?
bool g_ShowPhysics2DDebug = false;       // 2D 纰版挒浣撶嚎妗嗚皟璇曟樉绀?閿洏 T 閿垏鎹?
float g_FPS = 0.0f;                      // 骞虫粦甯х巼
float g_UIOpacity = 0.6f;                // 运行时全局 UI 不透明度（0.2 到 1.0），默认 60%

float GetUIOpacity() {
    return g_UIOpacity;
}

void SetUIOpacity(float opacity) {
    g_UIOpacity = std::clamp(opacity, 0.2f, 1.0f);
}

// 瀵煎嚭缁?Editor.dll 椤圭洰绠＄悊鍣?閫夋嫨椤圭洰鍚庡垏鎹㈤」鐩牴骞跺姞杞藉叾鍦烘櫙(鍚姩椤垫ā寮?銆?// 瀹氫箟鍦?Game.dll(Editor.dll 閾炬帴 Game.lib 璋冪敤;EngineMain.exe 浜﹀彲璋冪敤)銆? (encoding-repaired)
extern "C" MIKAN_API void MikanEngine_OpenProject(const char* dir)
{
    if (!dir) return;
    if (!ProjectManager::GetInstance().SetProjectRoot(dir)) {
        printf("[MikanEngine] OpenProject failed: %s\n", dir);
        return;
    }
    printf("[MikanEngine] Opening project, loading scene...\n");
    // Projectized project (project.json): validate manifest assets, then load manifest scene.
    const ProjectManifest& mf = ProjectManager::GetInstance().GetManifest();
    if (mf.valid && !mf.scene.empty()) {
        for (const auto& asset : mf.assets) {
            std::error_code ec;
            const std::string assetPath =
                ProjectManager::GetInstance().ResolveAssetPath(asset);
            if (!std::filesystem::exists(assetPath, ec)) {
                printf("[MikanEngine] Project asset MISSING (add to project.json assets[]?): %s\n", asset.c_str());
            }
        }
        std::string scenePath =
            ProjectManager::GetInstance().ResolveAssetPath(mf.scene);
        if (SceneManager::GetInstance().ChangeScene(scenePath)) {
            printf("[MikanEngine] Project scene loaded: %s\n", scenePath.c_str());
        } else {
            printf("[MikanEngine] Project scene FAILED: %s (fallback default)\n", scenePath.c_str());
            ECS::SceneECS::GetInstance().LoadDefaultScene();
        }
        Physics2DSystem::GetInstance().ClearBodies();
        g_ProjectSelectionPending = false;
        printf("[MikanEngine] Project opened: %s\n", dir);
        const std::string& sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
        if (!sceneGame.empty()) {
            if (auto* gm = Game::GameManager::GetInstance().Activate(sceneGame)) {
                gm->OnSceneLoaded();
            }
        }
        return;
    }
    ECS::SceneECS::GetInstance().LoadDefaultScene();
    Physics2DSystem::GetInstance().ClearBodies(); // 鍦烘櫙閲嶅缓, 娓呯悊鏃?2D 鍒氫綋
    g_ProjectSelectionPending = false;
    printf("[MikanEngine] Project opened: %s\n", dir);
    // 椤圭洰鍦烘櫙鑻ュ甫椤跺眰 "game" 閿?鑷姩婵€娲诲搴旀父鎴忔ā鍧? (encoding-repaired)
const std::string& sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
    if (!sceneGame.empty()) {
        if (auto* gm = Game::GameManager::GetInstance().Activate(sceneGame)) {
            gm->OnSceneLoaded();
        }
    }
}

// 瀵煎嚭缁?Editor.dll 椤圭洰绠＄悊鍣?鎵嬪姩瀵煎叆鍦烘櫙鏂囦欢(浠绘剰 .json),鍔犺浇鍚庢寜鍦烘櫙 "game" 閿嚜鍔ㄦ縺娲绘父鎴忋€?// 缁熶竴璧?SceneManager(瀹屾暣娓呯悊 + 鍔犺浇 + 鐡︾墖 + 婵€娲?+ OnSceneLoaded)
extern "C" MIKAN_API void MikanEngine_LoadSceneFile(const char* path)
{
    if (!path || !path[0]) return;
    if (!SceneManager::GetInstance().ChangeScene(path)) {
        printf("[MikanEngine] LoadSceneFile failed: %s\n", path);
    } else {
        printf("[MikanEngine] Scene imported: %s\n", path);
    }
    g_ProjectSelectionPending = false;
}

// 瀵煎嚭缁欑紪杈戝櫒:鐑噸杞藉綋鍓嶆父鎴忔彃浠?缂栬瘧鏂?DLL 鍚庤皟鐢?涓嶉噸鍚紩鎿?銆?// 椤诲湪娓告垙闈炶繍琛屾€佽皟鐢?杩愯涓嵏杞?DLL 浼氬穿婧?;閲嶈浇鍚庤嚜鍔ㄩ噸鏂版縺娲诲苟 OnSceneLoaded 閲嶇粦鍦烘櫙銆? (encoding-repaired)
extern "C" MIKAN_API bool MikanEngine_ReloadCurrentGame()
{
    auto* gm = Game::GameManager::GetInstance().GetCurrent();
    if (!gm) {
        printf("[MikanEngine] Reload: no active game\n");
        return false;
    }
    std::string name = gm->GetName();
    if (!Game::GameManager::GetInstance().ReloadPlugin(name)) {
        printf("[MikanEngine] Reload failed (not a plugin or missing): %s\n", name.c_str());
        return false;
    }
    if (auto* fresh = Game::GameManager::GetInstance().Activate(name)) {
        fresh->OnSceneLoaded();
        printf("[MikanEngine] Game plugin hot-reloaded: %s\n", name.c_str());
        return true;
    }
    return false;
}

// GPU skinning master switch (--cpu-skinning forces CPU fallback)
// NOTE: GPU skinning breaks after swapchain rebuild (VSync toggle) - skinned models
// become invisible. Temporarily defaulted to CPU skinning until the GPU path is fixed.
bool g_UseGpuSkinning = true;

// 模型纹理 mipmap 开关：true=三线性 mip（默认）；false=强制 level0（禁用 mip，模型纹理用 LinearNoMip 采样器）。
// 关闭后远处会锯齿闪烁（无 mip 滤波），但近处保持 level0 原分辨率清晰。
bool g_ModelMipmap = true;

// 模型 mip LOD 偏置：负值让采样偏向高分辨率 mip（过渡距离拉远——更远处才切到低 mip）。
// 近/中距离更清晰；代价是远处低 mip 覆盖不足可能轻微闪缩。
float g_ModelMipLodBias = -1.0f;

// 模型纹理环绕方式：默认 REPEAT（obj/gltf 未指定 sampler 时的标准，平铺纹理安全）。
// 纹理自身的环绕由模型/gltf 的 sampler 定义决定（per-texture，见 ModelLoader 的 gltf sampler 解析），
// 不要全局改 CLAMP（会破坏 UV>1 的平铺纹理）。
bool g_ModelAddressRepeat = true;
