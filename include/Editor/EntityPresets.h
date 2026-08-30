#pragma once
#include "ECS/Types.h"
#include <string>
#include <vector>
#include <functional>

namespace Editor {

// 场景树"创建对象"预设(纯数据表,定义于 Editor.dll):
// 创建 = CreateEmpty(基础组件) + 按 components 列表逐项经组件注册表添加 + customize 定制字段。
// 新增对象类别只需在 EntityPresets.cpp 中加一行数据,无需改 UI 代码。
struct EntityPreset {
    const char* displayName;              // 菜单显示名
    const char* category;                 // 菜单分组(如 "3D" / "2D")
    const char* defaultName;              // 实体默认名
    std::vector<std::string> components;  // 初始组件(typeid 名,经 ComponentRegistry 添加)
    std::function<void(ECS::Entity)> customize; // 可选:创建后定制字段/层级
};

// Editor.dll 内部 API,无需跨 DLL 导出宏
const std::vector<EntityPreset>& GetEntityPresets();

// 将编辑器中新建的 3D 实体放到编辑器相机正前方，并选中它。
// 2D 实体使用 Canvas 的局部坐标，不应调用此函数。
void PlaceInFrontOfCamera(ECS::Entity entity);

} // namespace Editor
