#pragma once
#include "ECS/Types.h"
#include "ECS/ComponentRegistry.h"

namespace ECS { class IScriptBehaviour; }

namespace Editor {

// 通用组件字段渲染器(字段反射 1a):
// 遍历组件的 FieldMeta 表,按 FieldType 分派到通用 ImGui 控件。
// 属性面板对"简单组件"直接调用本函数,替换按类型硬编码的编辑块;
// 复杂组件(Transform/Material/RigidBody/Sprite2D/Tween)仍使用各自的定制编辑器。
void RenderComponentFields(ECS::Entity entity, const ECS::ComponentMeta& meta);

// 脚本参数字段渲染器(Unity 式): 按 IScriptBehaviour::GetParamFields 的字段表,
// 以同样的 FieldType 分派渲染脚本实例的参数(写回即实时生效;保存场景时由序列化刷新 paramsJson)。
void RenderScriptParamFields(ECS::IScriptBehaviour* script);

}
