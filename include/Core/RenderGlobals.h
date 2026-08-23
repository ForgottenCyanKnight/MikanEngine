#pragma once
// RenderGlobals.h - renderer global objects and editor runtime settings (P0 refactor)
#include "Platform/Export.h"
#include <vulkan/vulkan.h>

// Forward declarations (avoid circular includes with the renderer headers)
class SceneRenderer;
class SkyboxRenderer;
class RenderTarget;
class FullscreenQuad;
class TexturePool;

// Runtime texture pool (owned by Game.dll; shared with the editor via Attach)
extern MIKAN_API TexturePool* g_TexturePool;
extern MIKAN_API SceneRenderer g_SceneRenderer;
extern MIKAN_API SkyboxRenderer g_SkyboxRenderer;
extern MIKAN_API RenderTarget g_SceneRenderTarget;
extern MIKAN_API RenderTarget g_GameRenderTarget;
extern MIKAN_API FullscreenQuad g_FullscreenQuad;
extern MIKAN_API FullscreenQuad g_SceneFilterQuad;   // 编辑器 SceneView final 后处理（黑白滤镜测试）
extern MIKAN_API FullscreenQuad g_GameFilterQuad;    // 编辑器 GameView final 后处理

// 配置驱动的后处理链（PostProcessChain）：Scene/Game/swapchain 各一条
class PostProcessChain;
extern MIKAN_API PostProcessChain g_SceneChain;
extern MIKAN_API PostProcessChain g_GameChain;
extern MIKAN_API PostProcessChain g_SwapChain;
extern MIKAN_API bool g_IsPaused;
extern MIKAN_API int g_HandBlockId;  // 手持方块 id（物品栏选中格 ↔ 放置联动）

// Editor runtime settings (synced from Editor.dll each frame; read by the render pipeline)
extern MIKAN_API bool g_ShowSceneView;
extern MIKAN_API bool g_ShowGameView;
extern MIKAN_API bool g_ShowGrid;   // 场景视图: 世界网格 + 原点坐标轴(由 Editor.dll 每帧同步)
extern MIKAN_API bool g_ShowPhysics2DDebug; // 2D 碰撞体线框调试显示(键盘 T 切换)
extern MIKAN_API bool g_ProjectSelectionPending; // 启动未指定 --project:等待项目管理器选择项目
extern MIKAN_API bool g_SceneIs2D;  // 场景模式:仅有 2D 相机且无主 3D 相机 -> 2D 游戏(渲染/编辑器按此切换)
extern MIKAN_API bool g_EnableZPrepass;   // z-prepass 开关（2026-08-11 参考版适配——蓝本默认开）
extern MIKAN_API bool g_ShowFPS;    // 游戏画面右上角显示 FPS(菜单按 F 键切换)
extern MIKAN_API bool g_UseGpuSkinning; // 蒙皮方案: GPU(UBO 固定64, 顶点着色器动态索引); false = CPU 蒙皮 fallback
extern MIKAN_API bool g_ModelMipmap;    // 模型纹理 mipmap 开关（true=三线性 mip；false=强制 level0 禁用 mip）
extern MIKAN_API float g_ModelMipLodBias; // 模型 mip LOD 偏置（负值=拉长过渡距离，更远处才切低 mip；默认 -1.0）
extern MIKAN_API bool g_ModelAddressRepeat; // 模型纹理环绕（true=REPEAT 平铺；false=CLAMP_TO_EDGE 消除边缘接缝黑线）
extern MIKAN_API float g_FPS;       // 平滑帧率(每帧由主循环更新)

// Runtime UI appearance. The settings overlay changes this value live; 3D
// materials and post-processing are intentionally unaffected.
extern MIKAN_API float g_UIOpacity;
MIKAN_API float GetUIOpacity();
MIKAN_API void SetUIOpacity(float opacity);
