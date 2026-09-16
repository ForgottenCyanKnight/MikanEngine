#pragma once
// GltfLights.h - glTF KHR_lights_punctual 内嵌灯光导入（近似的无阴影点光源）
// 模型文件内嵌灯光**不建实体**：按 modelPath 懒解析缓存；每帧由
// SceneCollector::CaptureRenderWorldFromECS 按模型实体的世界变换注入 RenderWorld。
// 好处：同一 glTF 拖两份到场景，灯光各随其实体变换；源文件即真源，不污染场景存档。
// 近似约定：castShadow=false（绕开 8 盏 cube shadow 上限）；Spot 以全向点光近似
// （渲染端点光 UBO 目前只消费 Point）；Directional 跳过（引擎方向光槽由场景/大气控制）。
#include "Platform/Export.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Rendering {

struct MIKAN_API ImportedModelLight {
    enum class Type { Point, Spot };
    Type type = Type::Point;
    std::string name;
    glm::vec3 color = glm::vec3(1.0f);   // glTF 线性 RGB
    float intensity = 1.0f;              // 点光 cd（glTF 物理量纲直传）
    float range = 0.0f;                  // 米；0 = 无限（注入时回退引擎默认）
    float innerConeAngle = 0.0f;         // 弧度（Spot 用，注入 Point 时忽略）
    float outerConeAngle = 0.785398f;    // 弧度
    glm::vec3 position = glm::vec3(0.0f);          // glTF 节点层级世界位置（Y-up）
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // 节点层级世界旋转
};

class MIKAN_API GltfLightRegistry {
public:
    static GltfLightRegistry& GetInstance();

    // 按 MeshComponent.modelPath 取内嵌灯光（懒解析 + mtime 失效；非 glTF / 无扩展返回空表）
    const std::vector<ImportedModelLight>& GetLights(const std::string& modelPath);

    void Clear();

private:
    GltfLightRegistry() = default;

    bool ParseGltfLights(const std::string& diskPath, std::vector<ImportedModelLight>& out);

    struct CacheEntry {
        std::vector<ImportedModelLight> lights;
        bool fileOk = false;     // 上次 mtime 读取是否成功
        long long mtimeTicks = 0; // file_clock 原生 tick（MSVC 纪元下可为负，勿以符号判断）
    };
    std::unordered_map<std::string, CacheEntry> m_cache; // key = 原始 modelPath
};

// 灯光局部旋转 × 实体世界矩阵（含缩放）：取世界旋转的正交归一部分再复合局部旋转
inline glm::quat ImportedLightWorldRotation(const glm::mat4& world, const glm::quat& local)
{
    glm::mat3 m(world);
    glm::vec3 c0 = glm::normalize(m[0]);
    glm::vec3 c1 = glm::normalize(m[1] - c0 * glm::dot(c0, m[1]));
    glm::vec3 c2 = glm::cross(c0, c1);
    return glm::quat(glm::mat3(c0, c1, c2)) * local;
}

} // namespace Rendering
