// ConsoleCommands.cpp - 运行时命令控制台后端实现（Game.dll）
#include "Core/ConsoleCommands.h"

#include "Core/Log.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "ECS/ScriptSystem.h"
#include "ECS/ComponentRegistry.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <glm/glm.hpp>

namespace Core {

namespace {

using ECS::Entity;
using ECS::INVALID_ENTITY;

void Emit(std::vector<ConsoleLine>& out, int level, const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out.push_back({ buf, level });
    switch (level) {
        case 1: LOGW("%s", buf); break;
        case 2: LOGE("%s", buf); break;
        default: LOGI("%s", buf); break;
    }
}

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 按空白切分；maxParts > 0 时最后一个 token 吞掉剩余全部（供 set 的字段值）
std::vector<std::string> Split(const std::string& s, int maxParts = 0)
{
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        if (i >= s.size()) break;
        if (maxParts > 0 && (int)parts.size() == maxParts - 1) {
            parts.push_back(Trim(s.substr(i)));
            break;
        }
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
        parts.push_back(s.substr(i, j - i));
        i = j;
    }
    return parts;
}

// 目标 token -> 实体："." = 当前选中；"#123"/纯数字 = 实体 id；否则按名称查找。
bool ResolveTarget(const std::string& token, Entity& out, std::vector<ConsoleLine>& outLines)
{
    auto& scene = ECS::SceneECS::GetInstance();
    Entity e = INVALID_ENTITY;
    if (token == "." || token == "sel") {
        e = scene.GetSelectedEntity();
        if (e == INVALID_ENTITY) {
            Emit(outLines, 1, "没有选中的实体（先 select 或在层级窗口点选）");
            return false;
        }
    } else if (token.size() > 1 && token[0] == '#') {
        e = (Entity)std::strtoul(token.c_str() + 1, nullptr, 10);
    } else if (!token.empty() && std::all_of(token.begin(), token.end(),
                 [](unsigned char c) { return std::isdigit(c); })) {
        e = (Entity)std::strtoul(token.c_str(), nullptr, 10);
    } else {
        e = scene.FindByName(token);
        if (e == INVALID_ENTITY) {
            Emit(outLines, 1, "找不到实体: %s", token.c_str());
            return false;
        }
    }
    if (!ECS::Coordinator::GetInstance().IsAlive(e)) {
        Emit(outLines, 1, "实体不存在或已销毁: %s", token.c_str());
        return false;
    }
    out = e;
    return true;
}

// 从 args[begin..] 解析 count 个 float，失败返回 false 并输出提示
bool ParseFloats(const std::vector<std::string>& args, int begin, int count,
                 float* v, std::vector<ConsoleLine>& out)
{
    if ((int)args.size() < begin + count) {
        Emit(out, 1, "缺少参数：需要 %d 个数值", count);
        return false;
    }
    for (int i = 0; i < count; ++i) {
        char* end = nullptr;
        v[i] = std::strtof(args[begin + i].c_str(), &end);
        if (end == args[begin + i].c_str()) {
            Emit(out, 1, "无效数值: %s", args[begin + i].c_str());
            return false;
        }
    }
    return true;
}

const char* FieldTypeName(ECS::FieldType t)
{
    switch (t) {
        case ECS::FieldType::Bool: return "bool";
        case ECS::FieldType::Int: return "int";
        case ECS::FieldType::Float: return "float";
        case ECS::FieldType::Vec2: return "vec2";
        case ECS::FieldType::Vec3: return "vec3";
        case ECS::FieldType::Vec4: return "vec4";
        case ECS::FieldType::Color3: return "color3";
        case ECS::FieldType::Color4: return "color4";
        case ECS::FieldType::QuatEuler: return "euler";
        case ECS::FieldType::String: return "string";
        case ECS::FieldType::Path: return "path";
        case ECS::FieldType::Enum: return "enum";
        default: return "hidden";
    }
}

// 字段当前值 -> 文本（base = 实例首地址）
std::string FieldValueText(const ECS::FieldMeta& f, const void* base)
{
    const unsigned char* p = (const unsigned char*)base + f.offset;
    char buf[160];
    switch (f.type) {
        case ECS::FieldType::Bool:
            snprintf(buf, sizeof(buf), "%s", *(const bool*)p ? "true" : "false"); break;
        case ECS::FieldType::Int:
            snprintf(buf, sizeof(buf), "%d", *(const int*)p); break;
        case ECS::FieldType::Enum:
            snprintf(buf, sizeof(buf), "%d", *(const int*)p); break;
        case ECS::FieldType::Float:
            snprintf(buf, sizeof(buf), "%.4g", (double)*(const float*)p); break;
        case ECS::FieldType::Vec2:
            snprintf(buf, sizeof(buf), "(%.4g, %.4g)", (double)((const glm::vec2*)p)->x,
                     (double)((const glm::vec2*)p)->y); break;
        case ECS::FieldType::Vec3:
        case ECS::FieldType::Color3:
            snprintf(buf, sizeof(buf), "(%.4g, %.4g, %.4g)", (double)((const glm::vec3*)p)->x,
                     (double)((const glm::vec3*)p)->y, (double)((const glm::vec3*)p)->z); break;
        case ECS::FieldType::Vec4:
        case ECS::FieldType::Color4:
            snprintf(buf, sizeof(buf), "(%.4g, %.4g, %.4g, %.4g)", (double)((const glm::vec4*)p)->x,
                     (double)((const glm::vec4*)p)->y, (double)((const glm::vec4*)p)->z,
                     (double)((const glm::vec4*)p)->w); break;
        case ECS::FieldType::QuatEuler:
            snprintf(buf, sizeof(buf), "(%.4g, %.4g, %.4g)",
                     (double)glm::degrees(glm::eulerAngles(*(const glm::quat*)p)).x,
                     (double)glm::degrees(glm::eulerAngles(*(const glm::quat*)p)).y,
                     (double)glm::degrees(glm::eulerAngles(*(const glm::quat*)p)).z); break;
        case ECS::FieldType::String:
            return (*(const std::string*)p);
        default:
            return "(内部字段)";
    }
    return buf;
}

// 按字段表写实例内存；成功返回 true
bool WriteFieldValue(const ECS::FieldMeta& f, void* base, const std::string& value,
                     std::vector<ConsoleLine>& out)
{
    unsigned char* p = (unsigned char*)base + f.offset;
    switch (f.type) {
        case ECS::FieldType::Bool:
            *(bool*)p = (ToLower(value) == "true" || value == "1");
            return true;
        case ECS::FieldType::Int:
        case ECS::FieldType::Enum:
            *(int*)p = (int)std::strtol(value.c_str(), nullptr, 0);
            return true;
        case ECS::FieldType::Float: {
            char* end = nullptr;
            *(float*)p = std::strtof(value.c_str(), &end);
            if (end == value.c_str()) break;
            return true;
        }
        case ECS::FieldType::Vec2:
        case ECS::FieldType::Vec3:
        case ECS::FieldType::Vec4:
        case ECS::FieldType::Color3:
        case ECS::FieldType::Color4: {
            int n = (f.type == ECS::FieldType::Vec2) ? 2
                  : (f.type == ECS::FieldType::Vec3 || f.type == ECS::FieldType::Color3) ? 3 : 4;
            float v[4];
            std::vector<std::string> toks = Split(value);
            bool ok = (int)toks.size() == n;
            if (ok) {
                for (int i = 0; i < n; ++i) {
                    char* end = nullptr;
                    v[i] = std::strtof(toks[i].c_str(), &end);
                    if (end == toks[i].c_str()) { ok = false; break; }
                }
            }
            if (!ok) {
                Emit(out, 1, "字段 %s 需要 %d 个数值（空格分隔）", f.name, n);
                return false;
            }
            if (n == 2) *(glm::vec2*)p = glm::vec2(v[0], v[1]);
            else if (n == 3) *(glm::vec3*)p = glm::vec3(v[0], v[1], v[2]);
            else *(glm::vec4*)p = glm::vec4(v[0], v[1], v[2], v[3]);
            return true;
        }
        case ECS::FieldType::QuatEuler: {
            std::vector<std::string> toks = Split(value);
            if (toks.size() != 3) {
                Emit(out, 1, "字段 %s 需要 3 个欧拉角数值（度）", f.name);
                return false;
            }
            glm::vec3 e(0.0f);
            for (int i = 0; i < 3; ++i) e[i] = std::strtof(toks[i].c_str(), nullptr);
            *(glm::quat*)p = glm::quat(glm::radians(e));
            return true;
        }
        case ECS::FieldType::String:
            *(std::string*)p = value;
            return true;
        default:
            Emit(out, 1, "字段 %s 类型(%s)不支持命令行写入", f.name, FieldTypeName(f.type));
            return false;
    }
    Emit(out, 1, "无效数值: %s", value.c_str());
    return false;
}

// 从 args[begin..] 用空格拼回一个字符串（set 的字段值原样保留）
std::string JoinFrom(const std::vector<std::string>& args, int begin)
{
    std::string s;
    for (int i = begin; i < (int)args.size(); ++i) {
        if (!s.empty()) s += ' ';
        s += args[i];
    }
    return s;
}

void CmdHelp(std::vector<ConsoleLine>& out)
{
    Emit(out, 0, "命令列表（目标 = 实体名称 | #id | . 表示当前选中）：");
    Emit(out, 0, "  ls [过滤]              列出实体（id 名称 [脚本]），可子串过滤");
    Emit(out, 0, "  select <目标>          选中实体    sel 显示当前选中");
    Emit(out, 0, "  spawn <类型> <名称> [脚本]  创建实体；类型: empty cube sphere plane");
    Emit(out, 0, "                             cylinder cone capsule torus pyramid camera light");
    Emit(out, 0, "  kill <目标>            删除实体");
    Emit(out, 0, "  tp <目标> <x> <y> <z>  设置位置");
    Emit(out, 0, "  rot <目标> <x> <y> <z> 设置旋转（欧拉角，度）");
    Emit(out, 0, "  scl <目标> <x> <y> <z> 设置缩放");
    Emit(out, 0, "  get <目标>             查看实体详情（transform / 脚本 / 参数）");
    Emit(out, 0, "  fields <目标>          列出脚本参数字段与当前值");
    Emit(out, 0, "  set <目标> <字段> <值> 写脚本参数（需播放态实例化，即时生效）");
    Emit(out, 0, "  attach <目标> <脚本名> 挂载脚本      detach <目标> 卸载脚本");
    Emit(out, 0, "  scripts                列出已注册脚本");
}

void CmdList(const std::vector<std::string>& args, std::vector<ConsoleLine>& out)
{
    std::string filter = args.empty() ? "" : ToLower(args[0]);
    const auto& entities = ECS::SceneECS::GetInstance().GetHierarchyEntities();
    auto& coord = ECS::Coordinator::GetInstance();
    int count = 0;
    for (Entity e : entities) {
        if (!coord.IsAlive(e)) continue;
        std::string name = ECS::SceneECS::GetInstance().GetName(e);
        if (!filter.empty() && ToLower(name).find(filter) == std::string::npos) continue;
        std::string script;
        if (coord.HasComponent<ECS::ScriptComponent>(e)) {
            const ECS::ScriptComponent& sc = coord.GetComponent<ECS::ScriptComponent>(e);
            if (!sc.scriptName.empty()) script = "  [" + sc.scriptName + "]";
        }
        Emit(out, 0, "  #%u  %s%s", (unsigned)e, name.c_str(), script.c_str());
        ++count;
    }
    Emit(out, 0, "共 %d 个实体", count);
}

void CmdSpawn(const std::vector<std::string>& args, std::vector<ConsoleLine>& out)
{
    if (args.size() < 2) {
        Emit(out, 1, "用法: spawn <类型> <名称> [脚本名]");
        return;
    }
    auto& scene = ECS::SceneECS::GetInstance();
    const std::string& type = ToLower(args[0]);
    const std::string& name = args[1];
    Entity e = INVALID_ENTITY;
    if (type == "empty") e = scene.CreateEmpty(name);
    else if (type == "cube") e = scene.CreateCube(name);
    else if (type == "sphere") e = scene.CreateSphere(name);
    else if (type == "plane") e = scene.CreatePlane(name);
    else if (type == "cylinder") e = scene.CreateCylinder(name);
    else if (type == "cone") e = scene.CreateCone(name);
    else if (type == "capsule") e = scene.CreateCapsule(name);
    else if (type == "torus") e = scene.CreateTorus(name);
    else if (type == "pyramid") e = scene.CreatePyramid(name);
    else if (type == "camera") e = scene.CreateCamera(name);
    else if (type == "light") e = scene.CreateLight(name);
    else {
        Emit(out, 1, "未知类型: %s（empty/cube/sphere/plane/cylinder/cone/capsule/torus/pyramid/camera/light）",
             args[0].c_str());
        return;
    }
    if (args.size() >= 3) {
        if (!ECS::ScriptSystem::GetInstance().AttachScript(e, args[2])) {
            Emit(out, 1, "脚本挂载失败: %s（未注册？）", args[2].c_str());
        }
    }
    scene.SetSelectedEntity(e);
    Emit(out, 0, "已创建 #%u %s%s（已选中）", (unsigned)e, name.c_str(),
         args.size() >= 3 ? "" : "，如需脚本请 attach");
}

void CmdGet(const std::vector<std::string>& args, std::vector<ConsoleLine>& out)
{
    Entity e;
    if (!ResolveTarget(args.empty() ? "." : args[0], e, out)) return;
    auto& coord = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();
    glm::vec3 p = scene.GetPosition(e);
    glm::vec3 r = scene.GetRotationEuler(e);
    glm::vec3 s = scene.GetScale(e);
    Emit(out, 0, "#%u %s", (unsigned)e, scene.GetName(e).c_str());
    Emit(out, 0, "  pos (%.3f, %.3f, %.3f)  rot (%.1f, %.1f, %.1f)  scale (%.3f, %.3f, %.3f)",
         (double)p.x, (double)p.y, (double)p.z,
         (double)r.x, (double)r.y, (double)r.z,
         (double)s.x, (double)s.y, (double)s.z);
    if (coord.HasComponent<ECS::ScriptComponent>(e)) {
        const ECS::ScriptComponent& sc = coord.GetComponent<ECS::ScriptComponent>(e);
        Emit(out, 0, "  script: %s", sc.scriptName.empty() ? "(无)" : sc.scriptName.c_str());
        if (!sc.paramsJson.empty())
            Emit(out, 0, "  params: %s", sc.paramsJson.c_str());
    }
    Emit(out, 0, "  components: %d", (int)coord.GetEntityComponentNames(e).size());
}

void CmdFields(const std::vector<std::string>& args, std::vector<ConsoleLine>& out)
{
    Entity e;
    if (!ResolveTarget(args.empty() ? "." : args[0], e, out)) return;
    auto& coord = ECS::Coordinator::GetInstance();
    if (!coord.HasComponent<ECS::ScriptComponent>(e)) {
        Emit(out, 1, "该实体没有脚本组件");
        return;
    }
    const ECS::ScriptComponent& sc = coord.GetComponent<ECS::ScriptComponent>(e);
    if (sc.scriptName.empty()) {
        Emit(out, 1, "该实体未挂脚本");
        return;
    }
    if (!sc.runtime) {
        Emit(out, 1, "脚本 %s 未实例化（需播放态；编辑态参数见属性面板）", sc.scriptName.c_str());
        return;
    }
    int count = 0;
    const ECS::FieldMeta* fields = sc.runtime->GetParamFields(count);
    Emit(out, 0, "%s 参数字段（%d 个）：", sc.scriptName.c_str(), count);
    for (int i = 0; i < count; ++i) {
        if (fields[i].type == ECS::FieldType::Hidden) continue;
        Emit(out, 0, "  %-24s %-7s = %s", fields[i].name, FieldTypeName(fields[i].type),
             FieldValueText(fields[i], sc.runtime).c_str());
    }
}

void CmdSet(const std::vector<std::string>& args, std::vector<ConsoleLine>& out)
{
    // set <目标> <字段> <值...>：目标与字段后全部剩余 token 作为值
    if (args.size() < 2) {
        Emit(out, 1, "用法: set <目标> <字段> <值>（例: set . speed 3.5）");
        return;
    }
    Entity e;
    if (!ResolveTarget(args[0], e, out)) return;
    auto& coord = ECS::Coordinator::GetInstance();
    if (!coord.HasComponent<ECS::ScriptComponent>(e)) {
        Emit(out, 1, "该实体没有脚本组件");
        return;
    }
    ECS::ScriptComponent& sc = coord.GetComponent<ECS::ScriptComponent>(e);
    if (!sc.runtime) {
        Emit(out, 1, "脚本 %s 未实例化（set 只在播放态生效）", sc.scriptName.c_str());
        return;
    }
    int count = 0;
    const ECS::FieldMeta* fields = sc.runtime->GetParamFields(count);
    const std::string& field = args[1];
    for (int i = 0; i < count; ++i) {
        if (field != fields[i].name) continue;
        std::string value = Trim(JoinFrom(args, 2));
        if (WriteFieldValue(fields[i], sc.runtime, value, out)) {
            Emit(out, 0, "%s.%s = %s", sc.scriptName.c_str(), field.c_str(),
                 FieldValueText(fields[i], sc.runtime).c_str());
        }
        return;
    }
    Emit(out, 1, "脚本 %s 没有字段 %s（fields 可列出全部）", sc.scriptName.c_str(), field.c_str());
}

} // namespace

void ConsoleCommands::Execute(const std::string& line, std::vector<ConsoleLine>& out)
{
    std::string trimmed = Trim(line);
    if (trimmed.empty()) return;
    std::vector<std::string> parts = Split(trimmed);
    const std::string cmd = ToLower(parts[0]);
    std::vector<std::string> args(parts.begin() + 1, parts.end());

    auto& scene = ECS::SceneECS::GetInstance();
    auto& coord = ECS::Coordinator::GetInstance();

    if (cmd == "help" || cmd == "?") {
        CmdHelp(out);
    } else if (cmd == "ls" || cmd == "list") {
        CmdList(args, out);
    } else if (cmd == "sel") {
        Entity e = scene.GetSelectedEntity();
        if (e == INVALID_ENTITY) Emit(out, 0, "当前未选中任何实体");
        else Emit(out, 0, "当前选中: #%u %s", (unsigned)e, scene.GetName(e).c_str());
    } else if (cmd == "select") {
        Entity e;
        if (ResolveTarget(args.empty() ? "." : args[0], e, out)) {
            scene.SetSelectedEntity(e);
            Emit(out, 0, "已选中 #%u %s", (unsigned)e, scene.GetName(e).c_str());
        }
    } else if (cmd == "spawn") {
        CmdSpawn(args, out);
    } else if (cmd == "kill" || cmd == "delete") {
        Entity e;
        if (!ResolveTarget(args.empty() ? "." : args[0], e, out)) return;
        std::string name = scene.GetName(e);
        scene.SetSelectedEntity(INVALID_ENTITY); // 先清选中，避免悬挂
        scene.DestroyEntity(e);
        Emit(out, 0, "已删除 #%u %s", (unsigned)e, name.c_str());
    } else if (cmd == "tp" || cmd == "pos") {
        Entity e;
        float v[3];
        if (!ResolveTarget(args.empty() ? "." : args[0], e, out)) return;
        if (!ParseFloats(args, 1, 3, v, out)) return;
        scene.SetPosition(e, glm::vec3(v[0], v[1], v[2]));
        Emit(out, 0, "#%u %s pos = (%.3f, %.3f, %.3f)", (unsigned)e,
             scene.GetName(e).c_str(), (double)v[0], (double)v[1], (double)v[2]);
    } else if (cmd == "rot") {
        Entity e;
        float v[3];
        if (!ResolveTarget(args.empty() ? "." : args[0], e, out)) return;
        if (!ParseFloats(args, 1, 3, v, out)) return;
        scene.SetRotationEuler(e, glm::vec3(v[0], v[1], v[2]));
        Emit(out, 0, "#%u %s rot = (%.1f, %.1f, %.1f)", (unsigned)e,
             scene.GetName(e).c_str(), (double)v[0], (double)v[1], (double)v[2]);
    } else if (cmd == "scl" || cmd == "scale") {
        Entity e;
        float v[3];
        if (!ResolveTarget(args.empty() ? "." : args[0], e, out)) return;
        if (!ParseFloats(args, 1, 3, v, out)) return;
        scene.SetScale(e, glm::vec3(v[0], v[1], v[2]));
        Emit(out, 0, "#%u %s scale = (%.3f, %.3f, %.3f)", (unsigned)e,
             scene.GetName(e).c_str(), (double)v[0], (double)v[1], (double)v[2]);
    } else if (cmd == "get") {
        CmdGet(args, out);
    } else if (cmd == "fields") {
        CmdFields(args, out);
    } else if (cmd == "set") {
        CmdSet(args, out);
    } else if (cmd == "attach") {
        if (args.size() < 2) { Emit(out, 1, "用法: attach <目标> <脚本名>"); return; }
        Entity e;
        if (!ResolveTarget(args[0], e, out)) return;
        if (ECS::ScriptSystem::GetInstance().AttachScript(e, args[1]))
            Emit(out, 0, "已挂载脚本 %s 到 #%u %s", args[1].c_str(), (unsigned)e,
                 scene.GetName(e).c_str());
        else
            Emit(out, 1, "挂载失败: %s（未注册？）", args[1].c_str());
    } else if (cmd == "detach") {
        Entity e;
        if (!ResolveTarget(args.empty() ? "." : args[0], e, out)) return;
        if (ECS::ScriptSystem::GetInstance().DetachScript(e))
            Emit(out, 0, "已卸载 #%u %s 的脚本", (unsigned)e, scene.GetName(e).c_str());
        else
            Emit(out, 1, "卸载失败（该实体没有脚本？）");
    } else if (cmd == "scripts") {
        const auto& names = ECS::ScriptSystem::GetInstance().GetRegisteredNames();
        for (const auto& n : names) Emit(out, 0, "  %s", n.c_str());
        Emit(out, 0, "共 %d 个已注册脚本", (int)names.size());
    } else {
        Emit(out, 1, "未知命令: %s（help 查看命令列表）", parts[0].c_str());
    }
}

} // namespace Core
