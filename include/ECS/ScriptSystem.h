#pragma once
// ScriptSystem.h - Unity 式 C++ 脚本组件系统
// 玩法逻辑不再硬编码 FindByName：把脚本类注册为工厂，场景 JSON 里实体挂 "script" 组件
//   {"scriptName":"RotateScript","params":{"speedDegPerSec":45}}
// 引擎反序列化时按 scriptName 创建实例并调 OnStart，每帧调 OnUpdate（受播放态 gate），
// 场景卸载/实体销毁时 OnDestroy。脚本宿主 = 游戏插件 DLL（加载时 REGISTER_SCRIPT 注册工厂）。
#include "Platform/Export.h"

#include "ECS/Types.h"
#include "ECS/ComponentRegistry.h"   // FieldMeta（脚本参数字段表复用组件反射机制）
#include <cstddef>                   // offsetof
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ECS {

// ===== 脚本行为接口（类似 Unity MonoBehaviour；跨 DLL 虚接口，宿主实现于游戏插件）=====
class MIKAN_API IScriptBehaviour {
public:
    virtual ~IScriptBehaviour() = default;

    // 脚本类名（与场景 JSON "scriptName" 一致）
    virtual const char* GetScriptName() const = 0;

    // 场景加载完成、实例创建后调用一次（entity = 挂载本脚本的实体）
    virtual void OnStart(Entity entity) = 0;
    // 每帧更新（仅运行态且未暂停时被引擎调用）
    virtual void OnUpdate(float deltaTime) = 0;
    // 场景卸载/实体销毁前调用（释放资源）
    virtual void OnDestroy() = 0;

    // 参数字段表（Unity 式 inspector 字段，复用 ComponentMeta 反射机制）：
    // 引擎按 offsetof 读写实例内存做场景序列化/属性面板。字段必须是 POD（或 std::string 用
    // 对象方法访问），offset 由脚本注册处（插件 DLL 编译单元）编译期计算。
    virtual const FieldMeta* GetParamFields(int& outCount) const { outCount = 0; return nullptr; }
};

using ScriptFactory = std::function<IScriptBehaviour*()>;

// ===== 脚本注册表 + 运行时实例管理（单例，定义于 Game.dll，插件经 DLL 导入）=====
class MIKAN_API ScriptSystem {
public:
    static ScriptSystem& GetInstance();

    // 注册脚本工厂（插件 DLL 静态初始化时经 REGISTER_SCRIPT 调用）
    void RegisterScript(const std::string& name, ScriptFactory factory);
    // 按名创建实例；未知脚本返回 nullptr
    IScriptBehaviour* CreateInstance(const std::string& name) const;
    // 已注册脚本名列表（编辑器脚本下拉用）
    const std::vector<std::string>& GetRegisteredNames() const { return m_registeredNames; }
    // 切换实体脚本：销毁旧实例 -> 更新 scriptName -> 按新名创建实例并 OnStart（编辑器换脚本）
    void RebindScript(Entity entity, const std::string& newName);

    // 场景加载完成：实例化脚本组件（quiet=true：插件 DLL 尚未加载时脚本工厂未注册属预期，
    // 引擎在 Activate 游戏模块后二次补齐并告警）
    void InstantiateAll(bool quietUnknown = false);
    // 动态挂载脚本：实体创建后可直接挂载，Update 期间自动延迟到 tick 结束。
    bool AttachScript(Entity entity, const std::string& name, const std::string& paramsJson = "{}");
    // 动态解绑脚本：Update 期间自动延迟，避免遍历实例表时失效。
    bool DetachScript(Entity entity);
    // 请求在当前 tick 结束后销毁实体，并先执行脚本 OnDestroy。
    bool QueueDestroy(Entity entity);
    // 每帧更新（主循环播放态块调用）
    void Update(float deltaTime);
    // 场景卸载/重载前：全部实例 OnDestroy 并销毁
    void DestroyAll();

    // 参数序列化：实例 <-> params JSON 对象（按脚本参数字段表）
    bool SerializeParams(IScriptBehaviour* script, std::string& outJson) const;
    bool DeserializeParams(IScriptBehaviour* script, const std::string& jsonStr) const;

private:
    ScriptSystem() = default;
    ~ScriptSystem() = default;
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    bool IsEntityAlive(Entity entity) const;
    bool AttachScriptImmediate(Entity entity, const std::string& name, const std::string& paramsJson);
    bool DetachScriptImmediate(Entity entity);
    void DestroyEntityImmediate(Entity entity);
    void FlushDeferredOperations();

    struct PendingAttach {
        Entity entity = INVALID_ENTITY;
        std::string name;
        std::string paramsJson;
    };

    std::unordered_map<std::string, ScriptFactory> m_factories;
    std::vector<std::string> m_registeredNames; // 已注册脚本名（编辑器下拉顺序）
    // entity -> 实例（ScriptComponent.runtime 亦指向同一实例）
    std::unordered_map<Entity, IScriptBehaviour*> m_instances;
    std::vector<PendingAttach> m_pendingAttaches;
    std::vector<Entity> m_pendingDetaches;
    std::vector<Entity> m_pendingDestroys;
    bool m_updating = false;
};

} // namespace ECS

// ===== 脚本注册宏（在游戏插件/引擎的 .cpp 里调用）=====
// 例：REGISTER_SCRIPT(RotateScript, "RotateScript");
#define REGISTER_SCRIPT(ClassName, ScriptName)                                 \
    static const bool s_registered_##ClassName = []() {                        \
        ::ECS::ScriptSystem::GetInstance().RegisterScript(                     \
            ScriptName, []() -> ::ECS::IScriptBehaviour* { return new ClassName(); }); \
        return true;                                                           \
    }()

// 脚本参数字段表条目（配合 GetParamFields 返回的静态数组使用）：
// 例：{ "speedDegPerSec", "旋转速度°/s", ECS::FieldType::Float, offsetof(RotateScript, speedDegPerSec) }
#define SCRIPT_FIELD(ClassName, fieldName, fieldType, label) \
    { #fieldName, label, ::ECS::FieldType::fieldType, offsetof(ClassName, fieldName) }
