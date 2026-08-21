#pragma once
// WorldGlobals.h - 体素世界全局访问点
// g_World 由 WorldSystem 管理（创建/销毁/每帧更新），
// WorldRenderer 和 Camera 碰撞检查通过它访问世界数据。
#include "Platform/Export.h"

class World;
class WorldRenderer;

extern MIKAN_API World* g_World;
extern MIKAN_API WorldRenderer* g_WorldRenderer;

// 体素世界运行时开关：false 时不创建/更新/渲染世界（见 EngineMain --no-voxel-world）
extern MIKAN_API bool g_EnableVoxelWorld;
