#pragma once
#include "Platform/Export.h"

#include "Types.h"
#include <array>
#include <cassert>

namespace ECS {

// 组件数组基类（用于多态存储）
class MIKAN_API IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void EntityDestroyed(Entity entity) = 0;
    virtual bool HasData(Entity entity) const = 0; // 供按 typeName 的非模板查询(编辑器增删组件用)
    // 供按 typeName 的通用字段编辑(反射渲染器用);无效实体返回 nullptr
    virtual void* GetRawData(Entity entity) = 0;
};

// 组件数组实现 - 使用连续内存存储（SoA）
// 实体→索引映射用 std::array<size_t, MAX_ENTITIES> + INVALID_INDEX 哨兵（Types.h 预留），
// 替代原 unordered_map:GetData/HasData 从"哈希+链遍历"降为一次数组索引+一次比较。
// 实体 ID 由 EntityManager 保证 < MAX_ENTITIES，故直接按 ID 索引安全；
// 越界 ID 在 HasData/EntityDestroyed 中做防御（与原 unordered_map 行为一致：视为无组件）。
template<typename T>
class ComponentArray : public IComponentArray {
public:
    ComponentArray() {
        // 全部槽位置为哨兵，区分"无组件"
        m_EntityToIndex.fill(INVALID_INDEX);
    }

    void InsertData(Entity entity, T component) {
        assert(entity < MAX_ENTITIES && "Entity out of range.");
        assert(m_EntityToIndex[entity] == INVALID_INDEX && "Component added to same entity more than once.");

        // 在数组末尾添加新元素
        size_t newIndex = m_Size;
        m_EntityToIndex[entity] = newIndex;
        m_IndexToEntity[newIndex] = entity;
        m_ComponentArray[newIndex] = std::move(component);
        ++m_Size;
    }

    void RemoveData(Entity entity) {
        assert(entity < MAX_ENTITIES && "Entity out of range.");
        assert(m_EntityToIndex[entity] != INVALID_INDEX && "Removing non-existent component.");

        // 将被删除元素与最后一个元素交换，保持数组紧凑
        size_t indexOfRemovedEntity = m_EntityToIndex[entity];
        size_t indexOfLastElement = m_Size - 1;
        m_ComponentArray[indexOfRemovedEntity] = std::move(m_ComponentArray[indexOfLastElement]);

        // 更新映射
        Entity entityOfLastElement = m_IndexToEntity[indexOfLastElement];
        m_EntityToIndex[entityOfLastElement] = indexOfRemovedEntity;
        m_IndexToEntity[indexOfRemovedEntity] = entityOfLastElement;

        // 清理
        m_EntityToIndex[entity] = INVALID_INDEX;
        // m_IndexToEntity[indexOfLastElement] 残留值无害（有效区以 m_Size 为界）

        --m_Size;
    }

    T& GetData(Entity entity) {
        assert(entity < MAX_ENTITIES && "Entity out of range.");
        assert(m_EntityToIndex[entity] != INVALID_INDEX && "Retrieving non-existent component.");
        return m_ComponentArray[m_EntityToIndex[entity]];
    }

    bool HasData(Entity entity) const override {
        if (entity >= MAX_ENTITIES) return false; // 防御：越界实体视为无组件
        return m_EntityToIndex[entity] != INVALID_INDEX;
    }

    // 反射渲染器用:按 typeName 拿到组件实例指针(无效返回 nullptr)
    void* GetRawData(Entity entity) override {
        if (entity >= MAX_ENTITIES || m_EntityToIndex[entity] == INVALID_INDEX) {
            return nullptr;
        }
        return &m_ComponentArray[m_EntityToIndex[entity]];
    }

    void EntityDestroyed(Entity entity) override {
        if (entity < MAX_ENTITIES && m_EntityToIndex[entity] != INVALID_INDEX) {
            RemoveData(entity);
        }
    }

    // 遍历所有组件（用于System）
    std::vector<Entity> GetEntities() const {
        std::vector<Entity> entities;
        entities.reserve(m_Size);
        for (size_t i = 0; i < m_Size; ++i) {
            entities.push_back(m_IndexToEntity[i]);
        }
        return entities;
    }

    size_t GetSize() const { return m_Size; }

private:
    // 组件数组 - 连续内存，缓存友好
    std::array<T, MAX_ENTITIES> m_ComponentArray;

    // 实体到数组索引的映射（INVALID_INDEX = 无组件；EntityManager 保证 entity < MAX_ENTITIES）
    std::array<size_t, MAX_ENTITIES> m_EntityToIndex;

    // 数组索引到实体的映射（仅 [0, m_Size) 有效）
    std::array<Entity, MAX_ENTITIES> m_IndexToEntity;

    // 有效组件数量
    size_t m_Size = 0;
};

} // namespace ECS
