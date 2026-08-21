// Engine Global Header (aggregate)
// Kept as a compatibility aggregate so existing files that include it keep working.
// New code should include the specific domain header instead:
//   Core/VulkanContext.h  Core/RenderGlobals.h  Core/InputGlobals.h  Core/PhysicsGlobals.h

#ifndef ENGINE_GLOBAL_H
#define ENGINE_GLOBAL_H

#include "VulkanContext.h"
#include "RenderGlobals.h"
#include "InputGlobals.h"
#include "PhysicsGlobals.h"

// Compatibility transitive includes (historically pulled in via EngineGlobal.h)
#include "Camera.h"
#include "InputController.h"
#include "PhysicsManager.h"
#include "ECS/PhysicsSystem.h"
#include "Rendering/TexturePool.h"
// 注：不再包含 Editor/GizmoMode.h —— 该头仅 Editor.dll 使用，Game.dll 运行时不应依赖编辑器类型

#endif // ENGINE_GLOBAL_H