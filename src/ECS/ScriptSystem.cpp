// ScriptSystem.cpp - 脚本组件系统实现（Unity 式 C++ 脚本挂载）
#include "ECS/ScriptSystem.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"

#include <cstdio>
#include <cctype>
#include <functional>
#include <glm/glm.hpp>
#include <sstream>
#include <vector>

namespace ECS {

ScriptSystem& ScriptSystem::GetInstance() {
    static ScriptSystem instance; // 定义于 Game.dll，插件 DLL 导入后访问同一实例
    return instance;
}

void ScriptSystem::RegisterScript(const std::string& name, ScriptFactory factory) {
    if (name.empty() || !factory) return;
    m_factories[name] = std::move(factory);
    // 维护下拉顺序列表（去重）
    bool exists = false;
    for (const auto& n : m_registeredNames) if (n == name) { exists = true; break; }
    if (!exists) m_registeredNames.push_back(name);
    printf("[ScriptSystem] Registered script: %s\n", name.c_str());
}

IScriptBehaviour* ScriptSystem::CreateInstance(const std::string& name) const {
    auto it = m_factories.find(name);
    if (it == m_factories.end()) return nullptr;
    return it->second();
}

// ===== params JSON 提取（轻量，兼容 SceneSerializer 的手写 JSON 风格）=====
static std::string ExtractJsonValue(const std::string& json, const std::string& key) {
    const std::string searchKey = "\"" + key + "\":";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    pos += searchKey.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '{') {
        int brace = 1; size_t end = pos + 1;
        while (end < json.size() && brace > 0) {
            if (json[end] == '{') brace++;
            else if (json[end] == '}') brace--;
            end++;
        }
        return json.substr(pos, end - pos);
    }
    if (json[pos] == '[') {
        int bracket = 1; size_t end = pos + 1;
        while (end < json.size() && bracket > 0) {
            if (json[end] == '[') bracket++;
            else if (json[end] == ']') bracket--;
            end++;
        }
        return json.substr(pos, end - pos);
    }
    if (json[pos] == '"') {
        size_t end = json.find('"', pos + 1);
        while (end != std::string::npos && end > 0 && json[end - 1] == '\\') end = json.find('"', end + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos + 1);
    }
    if (json.substr(pos, 4) == "true") return "true";
    if (json.substr(pos, 5) == "false") return "false";
    size_t end = pos;
    while (end < json.size() && (std::isdigit((unsigned char)json[end]) || json[end] == '.' || json[end] == '-' || json[end] == 'e' || json[end] == 'E')) end++;
    return json.substr(pos, end - pos);
}

static std::vector<float> ParseFloatArrayValue(const std::string& arrayStr) {
    std::vector<float> result;
    size_t start = arrayStr.find('[');
    size_t end = arrayStr.find(']');
    if (start == std::string::npos || end == std::string::npos) return result;
    std::stringstream ss(arrayStr.substr(start + 1, end - start - 1));
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (!token.empty()) {
            try { result.push_back(std::stof(token)); } catch (...) {}
        }
    }
    return result;
}

// ===== 参数序列化：实例字段表 -> params JSON 对象 =====
bool ScriptSystem::SerializeParams(IScriptBehaviour* script, std::string& outJson) const {
    int fieldCount = 0;
    const FieldMeta* fields = script ? script->GetParamFields(fieldCount) : nullptr;
    if (!fields || fieldCount <= 0) { outJson = "{}"; return true; }

    std::stringstream json;
    json << "{";
    bool first = true;
    for (int i = 0; i < fieldCount; ++i) {
        const FieldMeta& f = fields[i];
        if (f.type == FieldType::Hidden) continue;
        const void* fp = reinterpret_cast<const char*>(script) + f.offset;
        if (!first) json << ",";
        first = false;
        json << "\"" << f.name << "\":";
        switch (f.type) {
        case FieldType::Bool:   json << (*(const bool*)fp ? "true" : "false"); break;
        case FieldType::Int:    json << *(const int*)fp; break;
        case FieldType::Float:  json << *(const float*)fp; break;
        case FieldType::Vec2: {
            const auto& v = *(const glm::vec2*)fp;
            json << "[" << v.x << ", " << v.y << "]";
            break;
        }
        case FieldType::Vec3:
        case FieldType::Color3: {
            const auto& v = *(const glm::vec3*)fp;
            json << "[" << v.x << ", " << v.y << ", " << v.z << "]";
            break;
        }
        case FieldType::Vec4:
        case FieldType::Color4: {
            const auto& v = *(const glm::vec4*)fp;
            json << "[" << v.x << ", " << v.y << ", " << v.z << ", " << v.w << "]";
            break;
        }
        case FieldType::String: json << "\"" << *(const std::string*)fp << "\""; break;
        case FieldType::Enum:   json << *(const int*)fp; break;
        default: break;
        }
    }
    json << "}";
    outJson = json.str();
    return true;
}

// ===== 参数反序列化：params JSON 对象 -> 实例字段（缺失字段保留默认值）=====
bool ScriptSystem::DeserializeParams(IScriptBehaviour* script, const std::string& jsonStr) const {
    int fieldCount = 0;
    const FieldMeta* fields = script ? script->GetParamFields(fieldCount) : nullptr;
    if (!fields || fieldCount <= 0 || jsonStr.empty()) return true;

    for (int i = 0; i < fieldCount; ++i) {
        const FieldMeta& f = fields[i];
        if (f.type == FieldType::Hidden) continue;
        void* fp = reinterpret_cast<char*>(script) + f.offset;
        const std::string v = ExtractJsonValue(jsonStr, f.name);
        if (v.empty()) continue;
        switch (f.type) {
        case FieldType::Bool:   if (v == "true") *(bool*)fp = true; else if (v == "false") *(bool*)fp = false; break;
        case FieldType::Int:    *(int*)fp = std::stoi(v); break;
        case FieldType::Float:  *(float*)fp = std::stof(v); break;
        case FieldType::Vec2:
        case FieldType::Vec3:
        case FieldType::Color3: {
            auto arr = ParseFloatArrayValue(v);
            if (arr.size() >= 2) { float* p = (float*)fp; p[0] = arr[0]; p[1] = arr[1]; if (arr.size() >= 3) p[2] = arr[2]; }
            break;
        }
        case FieldType::Vec4:
        case FieldType::Color4: {
            auto arr = ParseFloatArrayValue(v);
            if (arr.size() >= 4) { float* p = (float*)fp; p[0] = arr[0]; p[1] = arr[1]; p[2] = arr[2]; p[3] = arr[3]; }
            break;
        }
        case FieldType::String: {
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') *(std::string*)fp = v.substr(1, v.size() - 2);
            break;
        }
        case FieldType::Enum:   *(int*)fp = std::stoi(v); break;
        default: break;
        }
    }
    return true;
}

// ===== 实例生命周期 =====
// 幂等补齐：场景加载与游戏插件加载的先后不定（插件 DLL 的 REGISTER_SCRIPT 在加载时注册），
// 可能被调用多次——只创建"有组件且 runtime 为空"的实体，已创建的不动。
void ScriptSystem::InstantiateAll(bool quietUnknown) {
    auto& scene = SceneECS::GetInstance();
    auto& coordinator = Coordinator::GetInstance();

    std::function<void(Entity)> visit = [&](Entity entity) {
        if (coordinator.HasComponent<ScriptComponent>(entity)) {
            auto& sc = coordinator.GetComponent<ScriptComponent>(entity);
            if (!sc.scriptName.empty() && sc.runtime == nullptr) {
                auto it = m_factories.find(sc.scriptName);
                if (it == m_factories.end()) {
                    if (!quietUnknown) {
                        fprintf(stderr, "[ScriptSystem] Unknown script '%s' (entity %s) - check REGISTER_SCRIPT in game plugin\n",
                                sc.scriptName.c_str(), scene.GetName(entity).c_str());
                    }
                } else {
                    IScriptBehaviour* inst = it->second();
                    sc.runtime = inst;
                    m_instances[entity] = inst;
                    DeserializeParams(inst, sc.paramsJson);
                    inst->OnStart(entity);
                }
            }
        }
        for (Entity child : scene.GetChildren(entity)) visit(child);
    };
    for (Entity root : scene.GetRootEntities()) visit(root);
}

void ScriptSystem::Update(float deltaTime) {
    for (auto& [entity, inst] : m_instances) {
        (void)entity;
        inst->OnUpdate(deltaTime);
    }
}

void ScriptSystem::RebindScript(Entity entity, const std::string& newName) {
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<ScriptComponent>(entity)) return;
    auto& sc = coordinator.GetComponent<ScriptComponent>(entity);

    // 销毁旧实例
    if (sc.runtime != nullptr) {
        auto it = m_instances.find(entity);
        if (it != m_instances.end()) {
            it->second->OnDestroy();
            delete it->second;
            m_instances.erase(it);
        }
        sc.runtime = nullptr;
    }
    sc.scriptName = newName;

    // 按新名创建（未知名/空名 -> 保持无实例）
    if (newName.empty()) return;
    auto fit = m_factories.find(newName);
    if (fit == m_factories.end()) {
        fprintf(stderr, "[ScriptSystem] Rebind: unknown script '%s' (entity %s)\n",
                newName.c_str(), SceneECS::GetInstance().GetName(entity).c_str());
        return;
    }
    IScriptBehaviour* inst = fit->second();
    sc.runtime = inst;
    m_instances[entity] = inst;
    DeserializeParams(inst, sc.paramsJson);
    inst->OnStart(entity);
}

void ScriptSystem::DestroyAll() {
    auto& coordinator = Coordinator::GetInstance();
    for (auto& [entity, inst] : m_instances) {
        inst->OnDestroy();
        delete inst;
        if (coordinator.HasComponent<ScriptComponent>(entity)) {
            coordinator.GetComponent<ScriptComponent>(entity).runtime = nullptr;
        }
    }
    m_instances.clear();
}

} // namespace ECS
