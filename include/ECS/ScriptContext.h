#pragma once

// ScriptContext.h - 面向 Agent 生成脚本的稳定玩法 SDK
//
// 脚本只需要持有一个 ScriptContext，即可访问常用的实体、变换、层级、输入和
// 组件反射能力。底层 Coordinator/SceneECS/InputSystem 仍然由 Game.dll 管理，
// 生成脚本不需要在每个文件里重复拼接单例调用。
//
// 组件字段接口使用场景协议里的 serializeKey 和 JSON 值，例如：
//   ctx.SetComponentField("render", "visible", "false");
//   ctx.SetComponentField("camera", "fov", "55.0");
//   ctx.SetComponentField("camera", "thirdPersonTargetOffset", "[0, 1.5, 0]");
// 变换字段优先使用 SetPosition/SetRotationEuler/SetScale，以保证缓存正确失效。

#include "Platform/Export.h"
#include "ECS/Types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace ECS {

class MIKAN_API ScriptContext {
public:
    // Agent 生成代码可以把该版本写入脚本元数据，后续 SDK 演进时可做兼容检查。
    static constexpr int ApiVersion = 1;

    explicit ScriptContext(Entity self = INVALID_ENTITY) : m_self(self) {}

    Entity Self() const { return m_self; }
    void SetSelf(Entity entity) { m_self = entity; }

    // target 省略或传 INVALID_ENTITY 时默认使用 Self()。
    bool IsAlive(Entity target = INVALID_ENTITY) const;

    // 实体与层级
    Entity Find(const std::string& name) const;
    std::vector<Entity> FindAll(const std::string& name) const;
    std::string Name(Entity target = INVALID_ENTITY) const;
    bool Rename(const std::string& name, Entity target = INVALID_ENTITY) const;
    Entity Parent(Entity target = INVALID_ENTITY) const;
    std::vector<Entity> Children(Entity target = INVALID_ENTITY) const;
    bool SetParent(Entity parent) const;                         // Self() 设为 parent 的子级
    bool SetParent(Entity child, Entity parent) const;
    bool RemoveParent(Entity target = INVALID_ENTITY) const;

    // 变换：角度单位为度；所有写操作走 SceneECS setter。
    glm::vec3 Position(Entity target = INVALID_ENTITY) const;
    glm::vec3 WorldPosition(Entity target = INVALID_ENTITY) const;
    bool SetPosition(const glm::vec3& position, Entity target = INVALID_ENTITY) const;
    glm::vec3 RotationEuler(Entity target = INVALID_ENTITY) const;
    bool SetRotationEuler(const glm::vec3& eulerDegrees, Entity target = INVALID_ENTITY) const;
    glm::vec3 Scale(Entity target = INVALID_ENTITY) const;
    bool SetScale(const glm::vec3& scale, Entity target = INVALID_ENTITY) const;

    // 可见性与运行时实体生命周期。
    bool IsVisible(Entity target = INVALID_ENTITY) const;
    bool SetVisible(bool visible, Entity target = INVALID_ENTITY) const;
    Entity CreateEmpty(const std::string& name = "Empty") const;
    Entity CreateCube(const std::string& name = "Cube") const;
    Entity CreateSphere(const std::string& name = "Sphere") const;
    bool Destroy(Entity target = INVALID_ENTITY) const;

    // 脚本动态挂载。Update 期间的挂载/解绑会延迟到当前 tick 结束，避免迭代器失效。
    bool AttachScript(const std::string& scriptName,
                      const std::string& paramsJson = "{}",
                      Entity target = INVALID_ENTITY) const;
    bool DetachScript(Entity target = INVALID_ENTITY) const;

    // 组件反射：componentKey 使用场景 JSON 的 serializeKey（如 render、camera、rigidBody）。
    bool HasComponent(const std::string& componentKey,
                      Entity target = INVALID_ENTITY) const;
    bool EnsureComponent(const std::string& componentKey,
                         Entity target = INVALID_ENTITY) const;
    bool RemoveComponent(const std::string& componentKey,
                         Entity target = INVALID_ENTITY) const;
    bool SetComponentField(const std::string& componentKey,
                           const std::string& fieldName,
                           const std::string& jsonValue,
                           Entity target = INVALID_ENTITY) const;
    bool GetComponentField(const std::string& componentKey,
                           const std::string& fieldName,
                           std::string& outJsonValue,
                           Entity target = INVALID_ENTITY) const;
    std::vector<std::string> GetComponentKeys(Entity target = INVALID_ENTITY) const;

    // 输入使用动作映射，Windows 键盘、Android 触控映射和 CPU 测试的 synthetic input 共用一套查询。
    bool IsDown(const std::string& action) const;
    bool IsPressed(const std::string& action) const;
    bool IsReleased(const std::string& action) const;
    float Axis(const std::string& negativeAction, const std::string& positiveAction) const;

    // 统一脚本日志，便于 Agent 从结构化测试日志中定位行为问题。
    void Log(const std::string& message) const;

private:
    Entity Resolve(Entity target) const {
        return target == INVALID_ENTITY ? m_self : target;
    }

    Entity m_self = INVALID_ENTITY;
};

} // namespace ECS
