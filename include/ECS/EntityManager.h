#pragma once
#include "Platform/Export.h"

#include "Types.h"
#include <queue>
#include <array>
#include <cassert>

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
        assert(m_LivingEntityCount < MAX_ENTITIES && "Too many entities in existence.");

        // 从队列中取出一个实体ID
        Entity id = m_AvailableEntities.front();
        m_AvailableEntities.pop();
        ++m_LivingEntityCount;
        return id;
    }

    void DestroyEntity(Entity entity) {
        assert(entity < MAX_ENTITIES && "Entity out of range.");

        // 重置签名
        m_Signatures[entity].reset();

        // 回收实体ID
        m_AvailableEntities.push(entity);
        --m_LivingEntityCount;
    }

    void SetSignature(Entity entity, Signature signature) {
        assert(entity < MAX_ENTITIES && "Entity out of range.");
        m_Signatures[entity] = signature;
    }

    Signature GetSignature(Entity entity) {
        assert(entity < MAX_ENTITIES && "Entity out of range.");
        return m_Signatures[entity];
    }

    uint32_t GetLivingEntityCount() const { return m_LivingEntityCount; }

private:
    // 可用实体ID队列
    std::queue<Entity> m_AvailableEntities{};

    // 实体签名数组
    std::array<Signature, MAX_ENTITIES> m_Signatures{};

    // 存活实体数量
    uint32_t m_LivingEntityCount = 0;
};

} // namespace ECS
