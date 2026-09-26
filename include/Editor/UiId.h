#pragma once
#include "Core/I18n.h"
#include "imgui/imgui.h"

#include <string>

// ============================================================================
// UiId — ImGui 控件 ID 与显示文本解耦（2026-09-26）
//
// ImGui 以标签文本哈希作为控件 ID：同一窗口内两个同名控件（如两处"清除"、
// 同类组件的同名字段）会共享 ID——点击/状态互相串扰。规则：
//   1. 相同文本、需要并存 → LabelId(文本, 唯一标记)：生成 "译文###标记"，
//      显示相同、ID 唯一（### 之后不显示，只参与哈希）；
//   2. 重复渲染的控件组（同类组件字段、列表行）→ UiScope RAII 按标记隔离，
//      组内控件 ID = 作用域 + 标签，组间天然不冲突；
//   3. 动态行（实体树/文件列表）优先用稳定主键 PushID（实体 id / 资源路径），
//      而不是显示名——显示名可重命名、可重复。
// 窗口标题走 I18n::WindowTitle（同理：显示名翻译 + ###稳定ID）。
// ============================================================================
namespace Editor {

// 相同文案 + 唯一标记 → "译文###标记"。适用于 Button/MenuItem/Selectable/
// Checkbox/CollapsingHeader/TabItem 等一切按标签哈希的控件。
inline std::string LabelId(const char* zh, const char* uniqueMarker) {
    return std::string(Tr(zh)) + "###" + uniqueMarker;
}

// RAII PushID：作用域内所有控件 ID 前缀隔离。
struct UiScope {
    explicit UiScope(const char* id) { ImGui::PushID(id); }
    explicit UiScope(int id) { ImGui::PushID(id); }
    explicit UiScope(void* id) { ImGui::PushID(id); }
    explicit UiScope(const std::string& id) { ImGui::PushID(id.c_str()); }
    ~UiScope() { ImGui::PopID(); }
    UiScope(const UiScope&) = delete;
    UiScope& operator=(const UiScope&) = delete;
};

} // namespace Editor
