#pragma once
// InputGlobals.h - input / camera / run-mode globals (P0 refactor: split from EngineGlobal.h)
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include "Camera.h"
#include "InputController.h"
#include "ECS/Types.h"

// Run mode enum (runtime mode: editor vs standalone game)
enum class RunMode {
    Editor,  // Editor mode: full UI, offscreen rendering
    Game     // Game mode: direct rendering, control panel floating only
};

// Gizmo operation mode
enum class GizmoMode {
    Translate = 0,
    Rotate = 1,
    Scale = 2
};

// Camera and input controller
extern MIKAN_API Camera                   g_Camera;
extern MIKAN_API InputController          g_InputController;
extern MIKAN_API RunMode                  g_RunMode;
// 编辑器 DLL 是否已附着（游戏模式时决定是否更新 GameRT 显示附件——编辑器全屏游戏视图消费它；headless 无编辑器则不执行）
extern MIKAN_API bool                     g_EditorActive;
extern MIKAN_API GizmoMode                g_GizmoMode;
extern MIKAN_API bool                     g_ShowAxis;
// 播放中且游戏视图为前台激活窗口（Editor.dll 帧末同步）。此时编辑器模式的
// WASD 输入切换到游戏内场景相机（isMainCamera 实体）；非播放态恒为 false，
// 游戏相机保持场景摆放位置不被移动。
extern MIKAN_API bool                     g_EditorPlayViewActive;

// Camera locked target (camera follow)
extern MIKAN_API ECS::Entity cameraLockedEntity;
