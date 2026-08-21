// ECSStatics.cpp - out-of-line singleton definitions
// Inline singletons would be duplicated per-DLL once Game.dll/Editor.dll are
// split; these out-of-line definitions keep exactly ONE instance (in Game.dll),
// which Editor.dll imports.
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "DescriptorSetCache.h"

namespace ECS {
Coordinator& Coordinator::GetInstance() {
    static Coordinator instance;
    return instance;
}
SceneECS& SceneECS::GetInstance() {
    static SceneECS instance;
    return instance;
}
} // namespace ECS

DescriptorSetCache& DescriptorSetCache::GetInstance() {
    static DescriptorSetCache instance;
    return instance;
}