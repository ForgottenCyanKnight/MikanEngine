#pragma once
#include "Platform/Export.h"

#include "Types.h"
#include "System.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <typeindex>
#include <cassert>

namespace ECS {

class MIKAN_API SystemManager {
public:
    template<typename T>
    std::shared_ptr<T> RegisterSystem() {
        std::string typeName = typeid(T).name();
        assert(m_Systems.find(typeName) == m_Systems.end() && "Registering system more than once.");

        auto system = std::make_shared<T>();
        m_Systems.insert({typeName, system});
        return system;
    }

    template<typename T>
    std::shared_ptr<T> GetSystem() {
        std::string typeName = typeid(T).name();
        assert(m_Systems.find(typeName) != m_Systems.end() && "System not registered before use.");
        return std::static_pointer_cast<T>(m_Systems[typeName]);
    }

    template<typename T>
    void SetSignature(Signature signature) {
        std::string typeName = typeid(T).name();
        assert(m_Systems.find(typeName) != m_Systems.end() && "System not registered before use.");
        m_Signatures.insert({typeName, signature});
    }

    void EntityDestroyed(Entity entity) {
        for (auto const& pair : m_Systems) {
            auto const& system = pair.second;
            auto it = system->m_Entities.find(entity);
            if (it != system->m_Entities.end()) {
                system->m_Entities.erase(it);
                // 实体销毁同样通知系统清理(如 PhysicsSystem 卸载 Jolt 刚体),
                // 否则 DestroyEntity(删除实体/重载场景)路径会泄漏刚体并留下失效映射
                system->OnEntityRemoved(entity);
            }
        }
    }

    void EntitySignatureChanged(Entity entity, Signature entitySignature) {
        for (auto const& pair : m_Systems) {
            auto const& type = pair.first;
            auto const& system = pair.second;
            auto const& systemSignature = m_Signatures[type];

            // 检查实体签名是否匹配系统签名
            bool hasAllComponents = (entitySignature & systemSignature) == systemSignature;
            bool currentlyHasEntity = system->m_Entities.find(entity) != system->m_Entities.end();

            if (hasAllComponents && !currentlyHasEntity) {
                // 实体现在有系统需要的所有组件，添加到系统
                system->m_Entities.insert(entity);
                system->OnEntityAdded(entity);
            } else if (!hasAllComponents && currentlyHasEntity) {
                // 实体缺少某些组件，从系统移除
                system->m_Entities.erase(entity);
                system->OnEntityRemoved(entity);
            }
        }
    }

private:
    // 系统签名映射 (std::string 与 ComponentManager 的 key 策略一致，避免跨 DLL 时
    // typeid(T).name() 裸指针地址不同导致注册/查询不一致)
    std::unordered_map<std::string, Signature> m_Signatures{};

    // 系统实例映射
    std::unordered_map<std::string, std::shared_ptr<System>> m_Systems{};
};

} // namespace ECS
