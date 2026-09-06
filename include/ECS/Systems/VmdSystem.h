#pragma once

#include "Platform/Export.h"
#include "Animation/VmdMotion.h"
#include "ECS/Types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ECS {

struct VmdPlayerComponent;

// VMD 运行时适配层：负责把独立的 Animation::VmdMotion 绑定到 ECS 实体。
// 组件挂在 PMX/PMD 上时驱动 ModelRenderer，挂在相机上时驱动 Transform/Camera。
class MIKAN_API VmdSystem {
public:
    static VmdSystem& GetInstance();

    void Update(float deltaTime);
    void Clear();

private:
    struct RuntimeState {
        std::shared_ptr<Animation::VmdMotion> motion;
        std::string resolvedPath;
        std::string loadError;
        float lastStartFrame = 0.0f;
        bool loadAttempted = false;
        bool initialized = false;
    };

    VmdSystem() = default;
    ~VmdSystem() = default;
    VmdSystem(const VmdSystem&) = delete;
    VmdSystem& operator=(const VmdSystem&) = delete;

    void VisitEntity(Entity entity, float deltaTime, std::unordered_set<Entity>& visited);
    bool EnsureMotion(Entity entity, const std::string& path, RuntimeState& state);
    void Advance(VmdPlayerComponent& player, RuntimeState& state, float deltaTime);
    void ApplyToModel(Entity entity, const VmdPlayerComponent& player,
                      const Animation::VmdMotion& motion);
    void ApplyToCamera(Entity entity, const VmdPlayerComponent& player,
                       const Animation::VmdMotion& motion);

    std::unordered_map<Entity, RuntimeState> m_runtime;
};

} // namespace ECS
