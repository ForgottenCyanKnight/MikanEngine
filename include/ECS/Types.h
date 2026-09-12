#pragma once
#include "Platform/Export.h"

#include <cstdint>
#include <bitset>
#include <limits>
#include <vector>

namespace ECS {

// 实体ID类型
using Entity = uint32_t;
const Entity MAX_ENTITIES = 10000;
const Entity INVALID_ENTITY = std::numeric_limits<Entity>::max();

// 组件数组索引哨兵（向量化 EntityToIndex 用）
constexpr size_t INVALID_INDEX = std::numeric_limits<size_t>::max();

// 组件类型ID
using ComponentType = uint8_t;
// Must cover every component registered by SceneECS::Init. Keep the bitset
// wide enough for editor/runtime additions; registration code also validates
// this limit so a future addition fails deterministically instead of writing
// past std::bitset's bounds.
constexpr ComponentType MAX_COMPONENTS = 64;

// Entity remains a compact index for ABI/source compatibility with the
// renderer, editor DLL and serialized scene format. Long-lived systems can
// pair it with this generation token to reject stale references when an index
// is recycled.
using EntityGeneration = uint32_t;
struct EntityHandle {
    Entity entity = INVALID_ENTITY;
    EntityGeneration generation = 0;

    bool IsValid() const { return entity != INVALID_ENTITY && generation != 0; }
    bool operator==(const EntityHandle& other) const {
        return entity == other.entity && generation == other.generation;
    }
    bool operator!=(const EntityHandle& other) const { return !(*this == other); }
};

// 系统类型ID
using SystemType = uint8_t;

// 实体签名 - 标记实体拥有哪些组件
using Signature = std::bitset<MAX_COMPONENTS>;

} // namespace ECS
