#pragma once
#include "Platform/Export.h"

#include "ECS/Types.h"
#include "ECS/Coordinator.h"
#include <string>
#include <vector>
#include <typeinfo>
#include <cassert>

namespace ECS {

// 字段类型(通用编辑器/序列化分派用)
enum class FieldType {
    Bool,
    Int,
    Float,
    Vec2,
    Vec3,
    Vec4,
    Color3,
    Color4,
    QuatEuler,   // 以欧拉角(vec3 度)编辑
    String,
    Path,        // 资源路径：只读显示 + 系统文件选择器
    Enum,        // enumNames 提供选项(以 nullptr 结尾)
    Hidden       // 运行时/内部字段,不编辑不显示
};

// 字段元数据:字段名(序列化 key)+ 显示名(编辑器中文标签)+ 类型 + 相对组件实例的字节偏移(offsetof)
struct FieldMeta {
    const char* name;            // 字段名(英文,序列化/代码用)
    const char* label;           // 显示名(中文,编辑器标签;nullptr 时回退到 name)
    FieldType type;
    size_t offset;
    const char* const* enumNames = nullptr; // Enum 用,以 nullptr 结尾
};

// 组件元数据(数据驱动,非 OOP):
// 组件本体仍是普通 struct,无基类无虚函数,ComponentArray 的 SoA 存储与系统签名匹配完全不受影响;
// 这里只存"描述 + 增删函数指针",供编辑器通用地展示/增删组件,避免属性面板按类型硬编码 if 块。
struct ComponentMeta {
    const char* typeName;        // typeid(T).name(),与 ComponentManager 的 key 一致
    const char* displayName;     // 编辑器显示名(如 "刚体")
    const char* category;        // 分组(如 "物理")
    bool userAddable = true;     // 是否允许用户手动"添加组件"
    bool userRemovable = true;   // 是否允许用户"移除组件"(内部/基础组件为 false)
    void (*addTo)(Entity);       // 给实体添加默认实例(函数指针,实例化于 Game.dll)
    void (*removeFrom)(Entity);  // 从实体移除组件
    const FieldMeta* fields = nullptr; // 字段表(通用属性面板渲染用;nullptr = 无通用字段)
    size_t fieldCount = 0;             // 字段数量
    const char* serializeKey = nullptr; // 序列化组件 key(如 "mesh",与旧存档一致);nullptr = 不入存档(运行时组件)
};

// 组件注册表(单例,定义在 Game.dll;Editor.dll 导入后只读遍历/查询)
class MIKAN_API ComponentRegistry {
public:
    static ComponentRegistry& GetInstance();

    // 模板注册助手:自动生成 add/remove 回调(模板实例化发生在调用处,即 Game.dll 内的 RegisterAllComponentMeta)
    template<typename T>
    static void RegisterComponent(const char* displayName, const char* category,
                                  bool userAddable = true, bool userRemovable = true,
                                  const FieldMeta* fields = nullptr, size_t fieldCount = 0,
                                  const char* serializeKey = nullptr) {
        ComponentMeta meta;
        meta.typeName = typeid(T).name();
        meta.displayName = displayName;
        meta.category = category;
        meta.userAddable = userAddable;
        meta.userRemovable = userRemovable;
        meta.addTo = &AddComponentToEntity<T>;
        meta.removeFrom = &RemoveComponentFromEntity<T>;
        meta.fields = fields;
        meta.fieldCount = fieldCount;
        meta.serializeKey = serializeKey;
        GetInstance().Register(meta);
    }

    void Register(const ComponentMeta& meta);
    const ComponentMeta* Find(const std::string& typeName) const;
    const std::vector<ComponentMeta>& GetAll() const { return m_Components; }

private:
    ComponentRegistry() = default;
    ~ComponentRegistry() = default;
    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;

    template<typename T>
    static void AddComponentToEntity(Entity entity) {
        Coordinator::GetInstance().AddComponent<T>(entity, T{});
    }
    template<typename T>
    static void RemoveComponentFromEntity(Entity entity) {
        Coordinator::GetInstance().RemoveComponent<T>(entity);
    }

    std::vector<ComponentMeta> m_Components;
};

// 注册全部内置组件元数据(由 SceneECS::Init 调用,Game 侧)
MIKAN_API void RegisterAllComponentMeta();

} // namespace ECS
