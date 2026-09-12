#pragma once
#include "Platform/Export.h"

#include "Types.h"
#include <queue>
#include <array>
#include <cassert>
#include <stdexcept>

namespace ECS {

class MIKAN_API EntityManager {
public:
    EntityManager() {
        // 初始化可用实体队列
        for (Entity entity = 0; entity < MAX_ENTITIES; ++entity) {
            m_AvailableEntities.push(entity);
        }
    }

    Entity CreateEntity() {
        if (m_LivingEntityCount >= MAX_ENTITIES || m_AvailableEntities.empty()) {
            throw std::overflow_error("ECS entity capacity exhausted");
        }

        // 从队列中取出一个实体ID
        Entity id = m_AvailableEntities.front();
        m_AvailableEntities.pop();
        m_Alive[id] = true;
        ++m_Generations[id];
        // Generation zero is reserved for an invalid/uninitialized handle.
        if (m_Generations[id] == 0) m_Generations[id] = 1;
        ++m_LivingEntityCount;
        return id;
    }

    bool DestroyEntity(Entity entity) {
        if (!IsAlive(entity)) {
            // Destruction is idempotent so stop/play, scene reload and
            // duplicate editor commands cannot enqueue the same ID twice.
            return false;
        }

        // 重置签名
        m_Signatures[entity].reset();
        m_Alive[entity] = false;

        // 回收实体ID
        m_AvailableEntities.push(entity);
        --m_LivingEntityCount;
        return true;
    }

    bool IsAlive(Entity entity) const {
        return entity < MAX_ENTITIES && m_Alive[entity];
    }

    EntityGeneration GetGeneration(Entity entity) const {
        return entity < MAX_ENTITIES ? m_Generations[entity] : 0;
    }

    EntityHandle GetHandle(Entity entity) const {
        if (!IsAlive(entity)) return {};
        return {entity, m_Generations[entity]};
    }

    bool IsAlive(EntityHandle handle) const {
        return handle.IsValid() && IsAlive(handle.entity) &&
               m_Generations[handle.entity] == handle.generation;
    }

    void SetSignature(Entity entity, Signature signature) {
        if (!IsAlive(entity)) return;
        m_Signatures[entity] = signature;
    }

    Signature GetSignature(Entity entity) {
        if (!IsAlive(entity)) return {};
        return m_Signatures[entity];
    }

    uint32_t GetLivingEntityCount() const { return m_LivingEntityCount; }

private:
    // 可用实体ID队列
    std::queue<Entity> m_AvailableEntities{};

    // 实体签名数组
    std::array<Signature, MAX_ENTITIES> m_Signatures{};

    // 存活位和代际号：实体索引可复用，但旧 EntityHandle 不会重新生效。
    std::array<bool, MAX_ENTITIES> m_Alive{};
    std::array<EntityGeneration, MAX_ENTITIES> m_Generations{};

    // 存活实体数量
    uint32_t m_LivingEntityCount = 0;
};

} // namespace ECS
