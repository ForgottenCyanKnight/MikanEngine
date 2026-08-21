#pragma once
#include "Platform/Export.h"

#include "Types.h"
#include "ComponentArray.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <typeindex>

namespace ECS {

class MIKAN_API ComponentManager {
public:
    template<typename T>
    void RegisterComponent() {
        std::string typeName = typeid(T).name();
        assert(m_ComponentTypes.find(typeName) == m_ComponentTypes.end() && "Registering component type more than once.");

        // 分配组件类型ID
        m_ComponentTypes.insert({typeName, m_NextComponentType});

        // 创建组件数组
        m_ComponentArrays.insert({typeName, std::make_shared<ComponentArray<T>>()});

        // ID → typeName 反向映射(注册顺序即 ID,供编辑器按签名反查组件名)
        m_TypeNamesById.push_back(typeName);

        ++m_NextComponentType;
    }

    template<typename T>
    ComponentType GetComponentType() {
        // 缓存优化:组件类型在 RegisterComponent 后 ID 固定不变,per-type 静态缓存把
        // 每次调用的字符串哈希/隐式 std::string 构造降为一次指针比较。
        // 函数内 static 对所有实例共享,故绑定实例指针:仅当缓存由本实例建立时才命中,
        // 多实例(测试/多场景)时自动重新解析,避免拿到其他实例的过期 ID。
        static ComponentManager* boundInstance = nullptr;
        static ComponentType cachedType = 0;
        static bool resolved = false;
        if (resolved && boundInstance == this) {
            return cachedType;
        }

        std::string typeName = typeid(T).name();
        auto it = m_ComponentTypes.find(typeName);
        assert(it != m_ComponentTypes.end() && "Component not registered before use.");
        if (it != m_ComponentTypes.end()) {
            boundInstance = this;
            cachedType = it->second;
            resolved = true;
            return cachedType;
        }
        return cachedType; // 未注册(release 下与原实现行为一致:返回默认 0)
    }

    template<typename T>
    void AddComponent(Entity entity, T component) {
        GetComponentArray<T>()->InsertData(entity, std::move(component));
    }

    template<typename T>
    void RemoveComponent(Entity entity) {
        GetComponentArray<T>()->RemoveData(entity);
    }

    template<typename T>
    T& GetComponent(Entity entity) {
        return GetComponentArray<T>()->GetData(entity);
    }

    template<typename T>
    bool HasComponent(Entity entity) {
        return GetComponentArray<T>()->HasData(entity);
    }

    void EntityDestroyed(Entity entity) {
        for (auto const& pair : m_ComponentArrays) {
            auto const& component = pair.second;
            component->EntityDestroyed(entity);
        }
    }

    // 按组件类型名(非模板)查询实体是否已挂载该组件(编辑器增删组件用)
    bool HasComponentByName(Entity entity, const std::string& typeName) {
        auto it = m_ComponentArrays.find(typeName);
        if (it == m_ComponentArrays.end()) {
            return false;
        }
        return it->second->HasData(entity);
    }

    // 按组件类型名(非模板)获取组件实例指针(反射通用渲染器用;无效返回 nullptr)
    void* GetComponentRaw(Entity entity, const std::string& typeName) {
        auto it = m_ComponentArrays.find(typeName);
        if (it == m_ComponentArrays.end()) {
            return nullptr;
        }
        return it->second->GetRawData(entity);
    }

    // 组件类型ID → typeid 名(注册顺序反查,编辑器通用展示用)
    const std::string& GetComponentTypeName(ComponentType typeId) const {
        assert(typeId < m_TypeNamesById.size() && "Component type id out of range.");
        return m_TypeNamesById[typeId];
    }

private:
    // 组件类型名称到ID的映射 (std::string so the key matches across DLLs)
    std::unordered_map<std::string, ComponentType> m_ComponentTypes{};

    // 组件类型名称到组件数组的映射 (std::string so the key matches across DLLs)
    std::unordered_map<std::string, std::shared_ptr<IComponentArray>> m_ComponentArrays{};

    // 下一个组件类型ID
    ComponentType m_NextComponentType{};

    // 注册顺序 → typeid 名(与 ComponentType ID 一一对应,供反向查询)
    std::vector<std::string> m_TypeNamesById{};

    template<typename T>
    std::shared_ptr<ComponentArray<T>> GetComponentArray() {
        // 缓存优化:同 GetComponentType,per-type 静态缓存数组指针,避免每次调用
        // 的字符串哈希/隐式 std::string 构造;并按实例绑定,多实例时自动重新解析。
        static ComponentManager* boundInstance = nullptr;
        static std::shared_ptr<ComponentArray<T>> cachedArray;
        if (boundInstance == this && cachedArray) {
            return cachedArray;
        }

        const char* typeName = typeid(T).name();
        auto it = m_ComponentArrays.find(typeName);
        assert(it != m_ComponentArrays.end() && "Component not registered before use.");
        if (it != m_ComponentArrays.end()) {
            boundInstance = this;
            cachedArray = std::static_pointer_cast<ComponentArray<T>>(it->second);
            return cachedArray;
        }
        return nullptr; // 未注册(release 下与原实现行为一致:返回空指针)
    }
};

} // namespace ECS
