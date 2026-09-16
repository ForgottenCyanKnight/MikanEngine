// GltfLights.cpp - glTF KHR_lights_punctual 解析与缓存
// 解析走 JsonLite（与 ModelLoader 的 KHR_texture_basisu 同一先例）：
// .gltf 整文件即 JSON；.glb 读 12 字节头后取 JSON chunk。
// 节点变换按 glTF 规范层级累加（M = T * R * S，支持 matrix 形式），灯光取世界 TRS。
#include "Rendering/GltfLights.h"

#include "Rendering/JsonLite.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace Rendering {

namespace {

// 读出 glTF 的 JSON 文本（.gltf 全文件 / .glb JSON chunk）
bool ReadGltfJson(const std::filesystem::path& path, std::string& outJson)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 12) return false;

    uint32_t magic = 0, version = 0, length = 0;
    std::memcpy(&magic, data.data(), 4);
    std::memcpy(&version, data.data() + 4, 4);
    std::memcpy(&length, data.data() + 8, 4);
    const uint32_t gltfMagic = 0x46546C67; // 'glTF'
    if (magic != gltfMagic) {
        outJson = std::move(data); // .gltf 文本
        return true;
    }
    if (version < 2 || data.size() < length) return false;

    // GLB: 12B header + chunks (length, type, payload)
    size_t off = 12;
    while (off + 8 <= data.size()) {
        uint32_t chunkLen = 0, chunkType = 0;
        std::memcpy(&chunkLen, data.data() + off, 4);
        std::memcpy(&chunkType, data.data() + off + 4, 4);
        off += 8;
        if (off + chunkLen > data.size()) return false;
        const uint32_t jsonType = 0x4E4F534A; // 'JSON'
        if (chunkType == jsonType) {
            outJson.assign(data.data() + off, chunkLen);
            return true;
        }
        off += chunkLen;
    }
    return false;
}

glm::vec3 ReadVec3(const JsonLite::Value* v, glm::vec3 fallback)
{
    if (!v || v->type != JsonLite::Value::Type::Array || v->arr.size() < 3) return fallback;
    return glm::vec3((float)v->arr[0].num, (float)v->arr[1].num, (float)v->arr[2].num);
}

// glTF rotation 四元数存储顺序 [x, y, z, w]；glm::quat 构造顺序 (w, x, y, z)
glm::quat ReadQuat(const JsonLite::Value* v)
{
    if (!v || v->type != JsonLite::Value::Type::Array || v->arr.size() < 4)
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::quat((float)v->arr[3].num, (float)v->arr[0].num,
                     (float)v->arr[1].num, (float)v->arr[2].num);
}

glm::mat4 NodeLocalMatrix(const JsonLite::Value& node)
{
    if (const JsonLite::Value* m = node.Get("matrix");
        m && m->type == JsonLite::Value::Type::Array && m->arr.size() >= 16) {
        // glTF matrix 为列主序 16 元组，glm::mat4 构造同为列主序，直取
        glm::mat4 out;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                out[c][r] = (float)m->arr[c * 4 + r].num;
        return out;
    }
    const glm::vec3 t = ReadVec3(node.Get("translation"), glm::vec3(0.0f));
    const glm::quat r = ReadQuat(node.Get("rotation"));
    const glm::vec3 s = ReadVec3(node.Get("scale"), glm::vec3(1.0f));
    glm::mat4 out = glm::mat4_cast(r);
    out[0] *= s.x; out[1] *= s.y; out[2] *= s.z;
    out[3] = glm::vec4(t, 1.0f);
    return out;
}

// 从世界矩阵提取刚性旋转（列归一化抵消缩放）——与头文件 ImportedLightWorldRotation 同法
glm::quat QuatFromWorldMatrix(const glm::mat4& m)
{
    glm::mat3 r(m);
    glm::vec3 c0 = glm::normalize(r[0]);
    glm::vec3 c1 = glm::normalize(r[1] - c0 * glm::dot(c0, r[1]));
    glm::vec3 c2 = glm::cross(c0, c1);
    return glm::quat(glm::mat3(c0, c1, c2));
}

// 递归走 glTF 场景节点树，累加世界变换，命中灯光节点即产出
void WalkNodes(const JsonLite::Value& root, const JsonLite::Value& nodes,
               const std::vector<unsigned>& indices, const glm::mat4& parentWorld,
               const std::vector<ImportedModelLight>& lightDefs,
               std::vector<ImportedModelLight>& out)
{
    for (unsigned idx : indices) {
        const JsonLite::Value* node = nodes.At(idx);
        if (!node || node->type != JsonLite::Value::Type::Object) continue;
        const glm::mat4 world = parentWorld * NodeLocalMatrix(*node);

        if (const JsonLite::Value* ext = node->Get("extensions")) {
            if (const JsonLite::Value* pl = ext->Get("KHR_lights_punctual")) {
                if (const JsonLite::Value* li = pl->Get("light");
                    li && li->type == JsonLite::Value::Type::Number &&
                    (size_t)li->num < lightDefs.size()) {
                    ImportedModelLight l = lightDefs[(size_t)li->num];
                    l.position = glm::vec3(world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                    l.rotation = QuatFromWorldMatrix(world);
                    out.push_back(std::move(l));
                }
            }
        }
        if (const JsonLite::Value* children = node->Get("children");
            children && children->type == JsonLite::Value::Type::Array) {
            std::vector<unsigned> childIdx;
            childIdx.reserve(children->arr.size());
            for (const auto& c : children->arr)
                if (c.type == JsonLite::Value::Type::Number) childIdx.push_back((unsigned)c.num);
            WalkNodes(root, nodes, childIdx, world, lightDefs, out);
        }
    }
}

// 返回 false = 文件不可读；true 时 mtimeTicks 为 file_clock 原生 tick 计数。
// 注意：MSVC 的 file_clock 纪元是 2440 年，count() 为负是正常的，不能用 >=0 判断成败。
bool FileMtimeTicks(const std::filesystem::path& p, long long& mtimeTicks)
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return false;
    mtimeTicks = t.time_since_epoch().count();
    return true;
}

} // namespace

GltfLightRegistry& GltfLightRegistry::GetInstance()
{
    static GltfLightRegistry instance;
    return instance;
}

void GltfLightRegistry::Clear()
{
    m_cache.clear();
}

const std::vector<ImportedModelLight>& GltfLightRegistry::GetLights(const std::string& modelPath)
{
    static const std::vector<ImportedModelLight> kEmpty;

    // 只对 glTF 家族扩展名尝试解析（.fbx 等直接空表，零开销）
    std::filesystem::path p(modelPath);
    std::string ext = p.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext != ".gltf" && ext != ".glb") return kEmpty;

    const std::string diskPath =
        ProjectManager::GetInstance().ResolveAssetPath(modelPath);
    if (diskPath.empty()) return kEmpty;

    long long mtimeTicks = 0;
    const bool fileOk = FileMtimeTicks(diskPath, mtimeTicks);
    auto it = m_cache.find(modelPath);
    if (it != m_cache.end() && it->second.fileOk == fileOk &&
        (!fileOk || it->second.mtimeTicks == mtimeTicks))
        return it->second.lights;

    CacheEntry entry;
    entry.fileOk = fileOk;
    entry.mtimeTicks = mtimeTicks;
    ParseGltfLights(diskPath, entry.lights);
    if (fileOk && !entry.lights.empty()) {
        LOGI("[GltfLights] %s: %d 个内嵌灯光（KHR_lights_punctual, 无阴影点光近似）",
             modelPath.c_str(), (int)entry.lights.size());
    }
    auto [ins, _] = m_cache.emplace(modelPath, std::move(entry));
    return ins->second.lights;
}

bool GltfLightRegistry::ParseGltfLights(const std::string& diskPath,
                                        std::vector<ImportedModelLight>& out)
{
    std::string json;
    if (!ReadGltfJson(diskPath, json)) return false;

    JsonLite::Value root;
    if (!JsonLite::Parse(json, root) ||
        root.type != JsonLite::Value::Type::Object) return false;

    // 1) 灯光定义表
    std::vector<ImportedModelLight> defs;
    if (const JsonLite::Value* ext = root.Get("extensions")) {
        if (const JsonLite::Value* pl = ext->Get("KHR_lights_punctual")) {
            if (const JsonLite::Value* lights = pl->Get("lights");
                lights && lights->type == JsonLite::Value::Type::Array) {
                defs.reserve(lights->arr.size());
                for (const auto& l : lights->arr) {
                    if (l.type != JsonLite::Value::Type::Object) continue;
                    ImportedModelLight d;
                    if (const JsonLite::Value* t = l.Get("name");
                        t && t->type == JsonLite::Value::Type::String) d.name = t->str;
                    std::string type = "point";
                    if (const JsonLite::Value* tv = l.Get("type");
                        tv && tv->type == JsonLite::Value::Type::String) type = tv->str;
                    if (type == "spot") {
                        d.type = ImportedModelLight::Type::Spot;
                    } else if (type == "point") {
                        d.type = ImportedModelLight::Type::Point;
                    } else {
                        continue; // directional：引擎方向光由场景/大气控制，跳过
                    }
                    d.color = ReadVec3(l.Get("color"), glm::vec3(1.0f));
                    if (const JsonLite::Value* iv = l.Get("intensity");
                        iv && iv->type == JsonLite::Value::Type::Number)
                        d.intensity = (float)iv->num;
                    if (const JsonLite::Value* rv = l.Get("range");
                        rv && rv->type == JsonLite::Value::Type::Number)
                        d.range = (float)rv->num;
                    if (d.type == ImportedModelLight::Type::Spot) {
                        if (const JsonLite::Value* sv = l.Get("spot");
                            sv && sv->type == JsonLite::Value::Type::Object) {
                            if (const JsonLite::Value* a = sv->Get("innerConeAngle");
                                a && a->type == JsonLite::Value::Type::Number)
                                d.innerConeAngle = (float)a->num;
                            if (const JsonLite::Value* a = sv->Get("outerConeAngle");
                                a && a->type == JsonLite::Value::Type::Number)
                                d.outerConeAngle = (float)a->num;
                        }
                    }
                    defs.push_back(std::move(d));
                }
            }
        }
    }
    if (defs.empty()) return false;

    // 2) 节点树：找到引用灯光的节点并取世界变换
    const JsonLite::Value* nodes = root.Get("nodes");
    if (!nodes || nodes->type != JsonLite::Value::Type::Array) return false;

    std::vector<unsigned> rootIndices;
    if (const JsonLite::Value* scenes = root.Get("scenes")) {
        int sceneIdx = 0;
        if (const JsonLite::Value* si = root.Get("scene");
            si && si->type == JsonLite::Value::Type::Number)
            sceneIdx = (int)si->num;
        if (const JsonLite::Value* scene = scenes->At((size_t)sceneIdx)) {
            if (const JsonLite::Value* sn = scene->Get("nodes");
                sn && sn->type == JsonLite::Value::Type::Array) {
                for (const auto& n : sn->arr)
                    if (n.type == JsonLite::Value::Type::Number)
                        rootIndices.push_back((unsigned)n.num);
            }
        }
    }
    if (rootIndices.empty()) {
        // 无 scenes 时兜底：把所有无父节点引用的节点当根（近似）
        std::vector<bool> referenced(nodes->arr.size(), false);
        for (const auto& n : nodes->arr) {
            if (const JsonLite::Value* ch = n.Get("children");
                ch && ch->type == JsonLite::Value::Type::Array)
                for (const auto& c : ch->arr)
                    if (c.type == JsonLite::Value::Type::Number &&
                        (size_t)c.num < referenced.size())
                        referenced[(size_t)c.num] = true;
        }
        for (size_t i = 0; i < referenced.size(); ++i)
            if (!referenced[i]) rootIndices.push_back((unsigned)i);
    }

    WalkNodes(root, *nodes, rootIndices, glm::mat4(1.0f), defs, out);
    return true;
}

} // namespace Rendering
