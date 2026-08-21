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
const ComponentType MAX_COMPONENTS = 32;

// 系统类型ID
using SystemType = uint8_t;

// 实体签名 - 标记实体拥有哪些组件
using Signature = std::bitset<MAX_COMPONENTS>;

} // namespace ECS
