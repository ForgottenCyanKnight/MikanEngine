#pragma once
#include "Platform/Export.h"

#include "Types.h"
#include "EntityManager.h"
#include "ComponentManager.h"
#include "SystemManager.h"
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

namespace ECS {

// ECS 协调器 - 统一管理实体、组件和系统
class MIKAN_API Coordinator {
public:
    static Coordinator& GetInstance();

    void Init() {
        m_EntityManager = std::make_unique<EntityManager>();
        m_ComponentManager = std::make_unique<ComponentManager>();
        m_SystemManager = std::make_unique<SystemManager>();
    }

    // 实体管理
    Entity CreateEntity() {
        return m_EntityManager->CreateEntity();
    }

    void DestroyEntity(Entity entity) {
        if (!m_EntityManager || !m_EntityManager->DestroyEntity(entity)) return;
        m_ComponentManager->EntityDestroyed(entity);
        m_SystemManager->EntityDestroyed(entity);
    }

    bool IsAlive(Entity entity) const {
        return m_EntityManager && m_EntityManager->IsAlive(entity);
    }

    EntityGeneration GetEntityGeneration(Entity entity) const {
        return m_EntityManager ? m_EntityManager->GetGeneration(entity) : 0;
    }

    EntityHandle GetEntityHandle(Entity entity) const {
        return m_EntityManager ? m_EntityManager->GetHandle(entity) : EntityHandle{};
    }

    bool IsAlive(EntityHandle handle) const {
        return m_EntityManager && m_EntityManager->IsAlive(handle);
    }

    // 组件管理
    template<typename T>
    void RegisterComponent() {
        m_ComponentManager->RegisterComponent<T>();
    }

    template<typename T>
    void AddComponent(Entity entity, T component) {
        EnsureEntityAlive(entity);
        m_ComponentManager->AddComponent<T>(entity, std::move(component));

        // 更新实体签名
        auto signature = m_EntityManager->GetSignature(entity);
        signature.set(m_ComponentManager->GetComponentType<T>(), true);
        m_EntityManager->SetSignature(entity, signature);

        // 通知系统实体签名改变
        m_SystemManager->EntitySignatureChanged(entity, signature);
    }

    template<typename T>
    void RemoveComponent(Entity entity) {
        EnsureEntityAlive(entity);
        m_ComponentManager->RemoveComponent<T>(entity);

        // 更新实体签名
        auto signature = m_EntityManager->GetSignature(entity);
        signature.set(m_ComponentManager->GetComponentType<T>(), false);
        m_EntityManager->SetSignature(entity, signature);

        // 通知系统实体签名改变
        m_SystemManager->EntitySignatureChanged(entity, signature);
    }

    template<typename T>
    T& GetComponent(Entity entity) {
        EnsureEntityAlive(entity);
        return m_ComponentManager->GetComponent<T>(entity);
    }

    template<typename T>
    bool HasComponent(Entity entity) {
        if (!IsAlive(entity)) return false;
        return m_ComponentManager->HasComponent<T>(entity);
    }

    template<typename T>
    ComponentType GetComponentType() {
        return m_ComponentManager->GetComponentType<T>();
    }

    Signature GetEntitySignature(Entity entity) {
        if (!IsAlive(entity)) return {};
        return m_EntityManager->GetSignature(entity);
    }

    // 按组件类型名(非模板)查询实体是否已挂载(编辑器通用增删组件用)
    bool HasComponentByName(Entity entity, const std::string& typeName) {
        return m_ComponentManager->HasComponentByName(entity, typeName);
    }

    // 按组件类型名(非模板)获取组件实例指针(反射通用渲染器用;无效返回 nullptr)
    void* GetComponentRaw(Entity entity, const std::string& typeName) {
        return m_ComponentManager->GetComponentRaw(entity, typeName);
    }

    // 获取实体已挂载的全部组件类型名列表(由签名位集反查,供编辑器通用展示组件)
    std::vector<std::string> GetEntityComponentNames(Entity entity) {
        std::vector<std::string> names;
        if (!IsAlive(entity)) return names;
        auto signature = m_EntityManager->GetSignature(entity);
        for (ComponentType t = 0; t < MAX_COMPONENTS; ++t) {
            if (signature.test(t)) {
                names.push_back(m_ComponentManager->GetComponentTypeName(t));
            }
        }
        return names;
    }

    // 系统管理
    template<typename T>
    std::shared_ptr<T> RegisterSystem() {
        return m_SystemManager->RegisterSystem<T>();
    }

    template<typename T>
    std::shared_ptr<T> GetSystem() {
        return m_SystemManager->GetSystem<T>();
    }

    template<typename T>
    void SetSystemSignature(Signature signature) {
        m_SystemManager->SetSignature<T>(signature);
    }

private:
    void EnsureEntityAlive(Entity entity) const {
        if (!IsAlive(entity)) {
            throw std::out_of_range("ECS entity is not alive");
        }
    }

    Coordinator() = default;
    ~Coordinator() = default;
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    std::unique_ptr<EntityManager> m_EntityManager;
    std::unique_ptr<ComponentManager> m_ComponentManager;
    std::unique_ptr<SystemManager> m_SystemManager;
};

} // namespace ECS
