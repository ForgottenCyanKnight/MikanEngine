// ComponentInspector.cpp - 通用组件字段渲染器(字段反射 1a)
// 按 FieldMeta 表的 FieldType 分派到通用 ImGui 控件。数据驱动:新增字段只需改字段表。
#include "Editor/ComponentInspector.h"
#include "Editor/AssetPathPicker.h"
#include "ECS/Coordinator.h"
#include "ECS/ScriptSystem.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui/imgui.h>
#include <cstring>
#include <string_view>

namespace Editor {

// 字符串编辑统一缓冲大小(TextComponent 文本可能较长)
static const int STR_BUF_SIZE = 2048;

static void RenderStringField(const char* label, void* fieldPtr) {
    std::string* str = static_cast<std::string*>(fieldPtr);
    static thread_local char buffer[STR_BUF_SIZE];
    strncpy(buffer, str->c_str(), STR_BUF_SIZE - 1);
    buffer[STR_BUF_SIZE - 1] = '\0';
    if (ImGui::InputText(label, buffer, STR_BUF_SIZE)) {
        *str = buffer;
    }
}

static AssetPathKind PathKindForField(const char* name) {
    if (!name) return AssetPathKind::Any;
    const std::string_view field(name);
    if (field == "motionPath") return AssetPathKind::Motion;
    if (field == "clip") return AssetPathKind::Audio;
    if (field == "voxPath") return AssetPathKind::Voxel;
    if (field == "tmxPath" || field == "tilemapFile") return AssetPathKind::Tilemap;
    if (field == "heightmapPath" || field == "layer0Path" ||
        field == "layer1Path" || field == "layer2Path" ||
        field == "layer3Path" || field == "controlMapPath") {
        return AssetPathKind::Texture;
    }
    if (field == "modelPath" || field == "collisionModelPath") {
        return AssetPathKind::Model;
    }
    return AssetPathKind::Any;
}

static void RenderEnumField(const char* label, void* fieldPtr, const char* const* names) {
    int idx = *static_cast<int*>(fieldPtr);
    int count = 0;
    while (names[count] != nullptr) ++count; // enumNames 以 nullptr 结尾
    if (ImGui::Combo(label, &idx, names, count)) {
        *static_cast<int*>(fieldPtr) = idx;
    }
}

static void RenderQuatEulerField(const char* label, void* fieldPtr) {
    glm::quat* quat = static_cast<glm::quat*>(fieldPtr);
    glm::vec3 euler = glm::degrees(glm::eulerAngles(*quat));
    if (ImGui::DragFloat3(label, &euler.x, 1.0f)) {
        *quat = glm::quat(glm::radians(euler));
    }
}

// 按单个 FieldMeta 渲染一个字段（组件字段与脚本参数共用）
static void RenderFieldByMeta(const ECS::FieldMeta& f, void* fieldPtr) {
    const char* label = f.label ? f.label : f.name; // 显示名(中文),无则回退字段名

    switch (f.type) {
    case ECS::FieldType::Bool: {
        bool v = *static_cast<bool*>(fieldPtr);
        if (ImGui::Checkbox(label, &v)) *static_cast<bool*>(fieldPtr) = v;
        break;
    }
    case ECS::FieldType::Int: {
        int v = *static_cast<int*>(fieldPtr);
        if (ImGui::DragInt(label, &v)) *static_cast<int*>(fieldPtr) = v;
        break;
    }
    case ECS::FieldType::Float: {
        float v = *static_cast<float*>(fieldPtr);
        if (ImGui::DragFloat(label, &v, 0.1f)) *static_cast<float*>(fieldPtr) = v;
        break;
    }
    case ECS::FieldType::Vec2:
        ImGui::DragFloat2(label, static_cast<float*>(fieldPtr));
        break;
    case ECS::FieldType::Vec3:
        ImGui::DragFloat3(label, static_cast<float*>(fieldPtr));
        break;
    case ECS::FieldType::Vec4:
        ImGui::DragFloat4(label, static_cast<float*>(fieldPtr));
        break;
    case ECS::FieldType::Color3:
        ImGui::ColorEdit3(label, static_cast<float*>(fieldPtr));
        break;
    case ECS::FieldType::Color4:
        ImGui::ColorEdit4(label, static_cast<float*>(fieldPtr));
        break;
    case ECS::FieldType::QuatEuler:
        RenderQuatEulerField(label, fieldPtr);
        break;
    case ECS::FieldType::String:
        RenderStringField(label, fieldPtr);
        break;
    case ECS::FieldType::Path:
        RenderAssetPathInput(label, *static_cast<std::string*>(fieldPtr),
                             PathKindForField(f.name));
        break;
    case ECS::FieldType::Enum:
        RenderEnumField(label, fieldPtr, f.enumNames);
        break;
    case ECS::FieldType::Hidden:
    default:
        break; // 运行时/内部字段,不显示
    }
}

void RenderComponentFields(ECS::Entity entity, const ECS::ComponentMeta& meta) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    void* comp = coordinator.GetComponentRaw(entity, meta.typeName);
    if (!comp) return;

    for (size_t i = 0; i < meta.fieldCount; ++i) {
        const ECS::FieldMeta& f = meta.fields[i];
        void* fieldPtr = static_cast<char*>(comp) + f.offset;
        RenderFieldByMeta(f, fieldPtr);
    }
}

void RenderScriptParamFields(ECS::IScriptBehaviour* script) {
    if (!script) return;
    int fieldCount = 0;
    const ECS::FieldMeta* fields = script->GetParamFields(fieldCount);
    if (!fields || fieldCount <= 0) {
        ImGui::TextDisabled("(该脚本没有可编辑参数)");
        return;
    }
    for (int i = 0; i < fieldCount; ++i) {
        const ECS::FieldMeta& f = fields[i];
        if (f.type == ECS::FieldType::Hidden) continue;
        void* fieldPtr = reinterpret_cast<char*>(script) + f.offset;
        RenderFieldByMeta(f, fieldPtr);
    }
}

} // namespace Editor
