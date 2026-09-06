// ScriptContext.cpp - Agent 生成玩法脚本的稳定引擎接口实现
#define GLM_ENABLE_EXPERIMENTAL
#include "ECS/ScriptContext.h"

#include "Core/InputSystem.h"
#include "Core/Log.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "ECS/ScriptSystem.h"

#include <glm/gtx/quaternion.hpp>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace ECS {
namespace {

const ComponentMeta* FindMetaByKey(const std::string& key) {
    if (key.empty()) return nullptr;
    for (const auto& meta : ComponentRegistry::GetInstance().GetAll()) {
        if (meta.serializeKey && key == meta.serializeKey) return &meta;
    }
    return nullptr;
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseFloat(const std::string& source, float& out) {
    const std::string value = Trim(source);
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);
    if (errno == ERANGE || end == value.c_str() || (end && *end != '\0') || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

bool ParseInt(const std::string& source, int& out) {
    const std::string value = Trim(source);
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || (end && *end != '\0')) return false;
    out = static_cast<int>(parsed);
    return true;
}

bool ParseBool(const std::string& source, bool& out) {
    const std::string value = Trim(source);
    if (value == "true") { out = true; return true; }
    if (value == "false") { out = false; return true; }
    return false;
}

bool ParseFloatArray(const std::string& source, std::vector<float>& out) {
    const std::string value = Trim(source);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;

    std::stringstream stream(value.substr(1, value.size() - 2));
    std::string token;
    while (std::getline(stream, token, ',')) {
        float number = 0.0f;
        if (!ParseFloat(token, number)) return false;
        out.push_back(number);
    }
    return !out.empty();
}

bool ParseString(const std::string& source, std::string& out) {
    const std::string value = Trim(source);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        // 为脚本调用保留一个方便的非 JSON 字符串形式；含引号时仍按 JSON 处理。
        out = value;
        return !out.empty();
    }

    out.clear();
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        char ch = value[i];
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (++i + 1 > value.size()) return false;
        ch = value[i];
        switch (ch) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: return false;
        }
    }
    return true;
}

std::string JsonQuote(const std::string& value) {
    std::string result = "\"";
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                std::ostringstream escaped;
                escaped << "\\u00" << std::hex << std::uppercase
                        << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
                result += escaped.str();
            } else {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    result += '"';
    return result;
}

const FieldMeta* FindField(const ComponentMeta* meta, const std::string& fieldName) {
    if (!meta || !meta->fields || fieldName.empty()) return nullptr;
    for (size_t i = 0; i < meta->fieldCount; ++i) {
        if (meta->fields[i].name && fieldName == meta->fields[i].name) return &meta->fields[i];
    }
    return nullptr;
}

void* GetFieldPointer(const ComponentMeta* meta, const FieldMeta* field, Entity entity) {
    if (!meta || !field) return nullptr;
    void* component = Coordinator::GetInstance().GetComponentRaw(entity, meta->typeName);
    if (!component) return nullptr;
    return static_cast<char*>(component) + field->offset;
}

bool SetFieldValue(const FieldMeta& field, void* fieldPointer, const std::string& jsonValue) {
    if (!fieldPointer || field.type == FieldType::Hidden) return false;
    try {
        switch (field.type) {
        case FieldType::Bool: {
            bool value = false;
            if (!ParseBool(jsonValue, value)) return false;
            *static_cast<bool*>(fieldPointer) = value;
            return true;
        }
        case FieldType::Int:
        case FieldType::Enum: {
            int value = 0;
            if (!ParseInt(jsonValue, value)) return false;
            *static_cast<int*>(fieldPointer) = value;
            return true;
        }
        case FieldType::Float: {
            float value = 0.0f;
            if (!ParseFloat(jsonValue, value)) return false;
            *static_cast<float*>(fieldPointer) = value;
            return true;
        }
        case FieldType::Vec2:
        case FieldType::Color3:
        case FieldType::Vec3:
        case FieldType::Vec4:
        case FieldType::Color4:
        case FieldType::QuatEuler: {
            std::vector<float> values;
            if (!ParseFloatArray(jsonValue, values)) return false;
            const size_t required = field.type == FieldType::Vec2 ? 2u
                : (field.type == FieldType::Vec4 || field.type == FieldType::Color4 ? 4u : 3u);
            if (values.size() != required) return false;
            if (field.type == FieldType::QuatEuler) {
                *static_cast<glm::quat*>(fieldPointer) = glm::quat(glm::radians(
                    glm::vec3(values[0], values[1], values[2])));
            } else {
                float* output = static_cast<float*>(fieldPointer);
                for (size_t i = 0; i < required; ++i) output[i] = values[i];
            }
            return true;
        }
        case FieldType::String:
        case FieldType::Path: {
            std::string value;
            if (!ParseString(jsonValue, value)) return false;
            *static_cast<std::string*>(fieldPointer) = std::move(value);
            return true;
        }
        default:
            return false;
        }
    } catch (...) {
        return false;
    }
}

bool GetFieldValue(const FieldMeta& field, const void* fieldPointer, std::string& out) {
    if (!fieldPointer || field.type == FieldType::Hidden) return false;
    std::ostringstream value;
    value << std::setprecision(9);
    switch (field.type) {
    case FieldType::Bool:
        out = *static_cast<const bool*>(fieldPointer) ? "true" : "false";
        return true;
    case FieldType::Int:
    case FieldType::Enum:
        value << *static_cast<const int*>(fieldPointer);
        break;
    case FieldType::Float:
        value << *static_cast<const float*>(fieldPointer);
        break;
    case FieldType::Vec2: {
        const auto& v = *static_cast<const glm::vec2*>(fieldPointer);
        value << "[" << v.x << "," << v.y << "]";
        break;
    }
    case FieldType::Vec3:
    case FieldType::Color3: {
        const auto& v = *static_cast<const glm::vec3*>(fieldPointer);
        value << "[" << v.x << "," << v.y << "," << v.z << "]";
        break;
    }
    case FieldType::Vec4:
    case FieldType::Color4: {
        const auto& v = *static_cast<const glm::vec4*>(fieldPointer);
        value << "[" << v.x << "," << v.y << "," << v.z << "," << v.w << "]";
        break;
    }
    case FieldType::QuatEuler: {
        const auto& q = *static_cast<const glm::quat*>(fieldPointer);
        const glm::vec3 euler = glm::degrees(glm::eulerAngles(q));
        value << "[" << euler.x << "," << euler.y << "," << euler.z << "]";
        break;
    }
    case FieldType::String:
    case FieldType::Path:
        out = JsonQuote(*static_cast<const std::string*>(fieldPointer));
        return true;
    default:
        return false;
    }
    out = value.str();
    return true;
}

} // namespace

bool ScriptContext::IsAlive(Entity target) const {
    const Entity entity = Resolve(target);
    if (entity == INVALID_ENTITY || entity >= MAX_ENTITIES) return false;
    return Coordinator::GetInstance().HasComponent<NameComponent>(entity);
}

Entity ScriptContext::Find(const std::string& name) const {
    return SceneECS::GetInstance().FindByName(name);
}

std::vector<Entity> ScriptContext::FindAll(const std::string& name) const {
    return SceneECS::GetInstance().QueryByName(name);
}

std::string ScriptContext::Name(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) ? SceneECS::GetInstance().GetName(entity) : std::string();
}

bool ScriptContext::Rename(const std::string& name, Entity target) const {
    const Entity entity = Resolve(target);
    if (!IsAlive(entity)) return false;
    SceneECS::GetInstance().SetName(entity, name);
    return true;
}

Entity ScriptContext::Parent(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) ? SceneECS::GetInstance().GetParent(entity) : INVALID_ENTITY;
}

std::vector<Entity> ScriptContext::Children(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) ? SceneECS::GetInstance().GetChildren(entity) : std::vector<Entity>();
}

bool ScriptContext::SetParent(Entity parent) const {
    return SetParent(m_self, parent);
}

bool ScriptContext::SetParent(Entity child, Entity parent) const {
    if (!IsAlive(child) || (parent != INVALID_ENTITY && !IsAlive(parent))) return false;
    if (parent == INVALID_ENTITY) SceneECS::GetInstance().RemoveParent(child);
    else SceneECS::GetInstance().SetParent(child, parent);
    return SceneECS::GetInstance().GetParent(child) == parent;
}

bool ScriptContext::RemoveParent(Entity target) const {
    const Entity entity = Resolve(target);
    if (!IsAlive(entity)) return false;
    SceneECS::GetInstance().RemoveParent(entity);
    return SceneECS::GetInstance().GetParent(entity) == INVALID_ENTITY;
}

glm::vec3 ScriptContext::Position(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) ? SceneECS::GetInstance().GetPosition(entity) : glm::vec3(0.0f);
}

glm::vec3 ScriptContext::WorldPosition(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) ? SceneECS::GetInstance().GetWorldPosition(entity) : glm::vec3(0.0f);
}

bool ScriptContext::SetPosition(const glm::vec3& position, Entity target) const {
    const Entity entity = Resolve(target);
    if (!IsAlive(entity)) return false;
    SceneECS::GetInstance().SetPosition(entity, position);
    return true;
}

glm::vec3 ScriptContext::RotationEuler(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) ? SceneECS::GetInstance().GetRotationEuler(entity) : glm::vec3(0.0f);
}

bool ScriptContext::SetRotationEuler(const glm::vec3& eulerDegrees, Entity target) const {
    const Entity entity = Resolve(target);
    if (!IsAlive(entity)) return false;
    SceneECS::GetInstance().SetRotationEuler(entity, eulerDegrees);
    return true;
}

glm::vec3 ScriptContext::Scale(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) ? SceneECS::GetInstance().GetScale(entity) : glm::vec3(1.0f);
}

bool ScriptContext::SetScale(const glm::vec3& scale, Entity target) const {
    const Entity entity = Resolve(target);
    if (!IsAlive(entity)) return false;
    SceneECS::GetInstance().SetScale(entity, scale);
    return true;
}

bool ScriptContext::IsVisible(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) && SceneECS::GetInstance().IsVisible(entity);
}

bool ScriptContext::SetVisible(bool visible, Entity target) const {
    const Entity entity = Resolve(target);
    if (!IsAlive(entity)) return false;
    SceneECS::GetInstance().SetVisible(entity, visible);
    return true;
}

Entity ScriptContext::CreateEmpty(const std::string& name) const {
    return SceneECS::GetInstance().CreateEmpty(name);
}

Entity ScriptContext::CreateCube(const std::string& name) const {
    return SceneECS::GetInstance().CreateCube(name);
}

Entity ScriptContext::CreateSphere(const std::string& name) const {
    return SceneECS::GetInstance().CreateSphere(name);
}

bool ScriptContext::Destroy(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) && ScriptSystem::GetInstance().QueueDestroy(entity);
}

bool ScriptContext::AttachScript(const std::string& scriptName,
                                 const std::string& paramsJson, Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) && ScriptSystem::GetInstance().AttachScript(entity, scriptName, paramsJson);
}

bool ScriptContext::DetachScript(Entity target) const {
    const Entity entity = Resolve(target);
    return IsAlive(entity) && ScriptSystem::GetInstance().DetachScript(entity);
}

bool ScriptContext::HasComponent(const std::string& componentKey, Entity target) const {
    const Entity entity = Resolve(target);
    const ComponentMeta* meta = FindMetaByKey(componentKey);
    return IsAlive(entity) && meta && Coordinator::GetInstance().GetComponentRaw(entity, meta->typeName) != nullptr;
}

bool ScriptContext::EnsureComponent(const std::string& componentKey, Entity target) const {
    const Entity entity = Resolve(target);
    const ComponentMeta* meta = FindMetaByKey(componentKey);
    if (!IsAlive(entity) || !meta) return false;
    if (Coordinator::GetInstance().GetComponentRaw(entity, meta->typeName) != nullptr) return true;
    if (!meta->userAddable || !meta->addTo) return false;
    meta->addTo(entity);
    return Coordinator::GetInstance().GetComponentRaw(entity, meta->typeName) != nullptr;
}

bool ScriptContext::RemoveComponent(const std::string& componentKey, Entity target) const {
    const Entity entity = Resolve(target);
    const ComponentMeta* meta = FindMetaByKey(componentKey);
    if (!IsAlive(entity) || !meta || !meta->userRemovable || !meta->removeFrom) return false;
    if (componentKey == "script") return ScriptSystem::GetInstance().DetachScript(entity);
    if (!Coordinator::GetInstance().GetComponentRaw(entity, meta->typeName)) return false;
    meta->removeFrom(entity);
    return Coordinator::GetInstance().GetComponentRaw(entity, meta->typeName) == nullptr;
}

bool ScriptContext::SetComponentField(const std::string& componentKey,
                                      const std::string& fieldName,
                                      const std::string& jsonValue,
                                      Entity target) const {
    const Entity entity = Resolve(target);
    const ComponentMeta* meta = FindMetaByKey(componentKey);
    const FieldMeta* field = FindField(meta, fieldName);
    if (!IsAlive(entity) || !field) return false;
    return SetFieldValue(*field, GetFieldPointer(meta, field, entity), jsonValue);
}

bool ScriptContext::GetComponentField(const std::string& componentKey,
                                      const std::string& fieldName,
                                      std::string& outJsonValue,
                                      Entity target) const {
    outJsonValue.clear();
    const Entity entity = Resolve(target);
    const ComponentMeta* meta = FindMetaByKey(componentKey);
    const FieldMeta* field = FindField(meta, fieldName);
    if (!IsAlive(entity) || !field) return false;
    const void* fieldPointer = GetFieldPointer(meta, field, entity);
    return GetFieldValue(*field, fieldPointer, outJsonValue);
}

std::vector<std::string> ScriptContext::GetComponentKeys(Entity target) const {
    const Entity entity = Resolve(target);
    std::vector<std::string> keys;
    if (!IsAlive(entity)) return keys;
    for (const auto& meta : ComponentRegistry::GetInstance().GetAll()) {
        if (meta.serializeKey && Coordinator::GetInstance().GetComponentRaw(entity, meta.typeName)) {
            keys.emplace_back(meta.serializeKey);
        }
    }
    return keys;
}

bool ScriptContext::IsDown(const std::string& action) const {
    return Input::InputSystem::GetInstance().IsDown(action.c_str());
}

bool ScriptContext::IsPressed(const std::string& action) const {
    return Input::InputSystem::GetInstance().IsPressed(action.c_str());
}

bool ScriptContext::IsReleased(const std::string& action) const {
    return Input::InputSystem::GetInstance().IsReleased(action.c_str());
}

float ScriptContext::Axis(const std::string& negativeAction,
                          const std::string& positiveAction) const {
    return Input::InputSystem::GetInstance().GetAxis(negativeAction.c_str(), positiveAction.c_str());
}

void ScriptContext::Log(const std::string& message) const {
    LOGI("[ScriptContext][entity=%u][name=%s] %s",
         static_cast<unsigned>(m_self), Name().c_str(), message.c_str());
}

} // namespace ECS
