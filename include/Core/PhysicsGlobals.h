#pragma once
// PhysicsGlobals.h - physics system globals (P0 refactor: split from EngineGlobal.h)
#include "Platform/Export.h"
#include <memory>
#include "PhysicsManager.h"
#include "ECS/PhysicsSystem.h"

// Physics manager (Jolt) and ECS physics system
extern class MIKAN_API Physics::PhysicsManager g_PhysicsManager;
extern MIKAN_API std::shared_ptr<class ECS::PhysicsSystem> g_PhysicsSystemPtr;