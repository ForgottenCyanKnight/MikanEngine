#pragma once
#include "Platform/Export.h"

#include "Types.h"
#include <set>

namespace ECS {

class MIKAN_API System {
public:
    virtual ~System() = default;

    // 系统关注的实体集合
    std::set<Entity> m_Entities;

    // 每帧更新
    virtual void Update(float deltaTime) {}

    // 实体添加/移除时的回调
    virtual void OnEntityAdded(Entity entity) {}
    virtual void OnEntityRemoved(Entity entity) {}
};

} // namespace ECS
