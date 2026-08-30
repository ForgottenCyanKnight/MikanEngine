#pragma once
#include "Platform/Export.h"
#ifndef MODEL_LOADER_H
#define MODEL_LOADER_H

#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/packing.hpp>

#ifdef __ANDROID__
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>
#include <SDL3/SDL.h>
#endif

// ===== 压缩顶点结构（2026-08-06：88B → 32B，移动端带宽优化）=====
// Position 保留 float3（精度关键）；法线/切线 SNORM int8（驱动自动归一化）；UV half float；
// 骨骼 ID uint8（0xFF=无骨骼，shader clamp）；权重 UNORM uint8（量化 0-255）。Bitangent 不再存储（shader 由 cross(normal,tangent)*w 推导）。
struct MIKAN_API Vertex {
    glm::vec3   Position;       // 12B  float（保留精度）
    glm::i8vec4 Normal;         // 4B   SNORM（xyzw；w 未用）
    glm::u16vec2 TexCoords;     // 4B   HALF float（R16G16_SFLOAT）
    glm::i8vec4 Tangent;        // 4B   SNORM（xyz=切线方向, w=±1 手性）
    glm::u8vec4 BoneIDs;        // 4B   UINT8 ×4（0xFF=无骨骼，shader clamp 到 MAX_BONES-1）
    glm::u8vec4 BoneWeights;    // 4B   UNORM ×4（量化 0-255，shader 驱动自动归一化 0..1）
};  // 共 32B

// ===== 顶点打包/解包辅助（压缩编码）=====
// SNORM 编码：[-1,1] → int8（±127）；w 可选（tangent 手性）
inline glm::i8vec4 PackSnorm3(const glm::vec3& v, float w = 0.0f) {
    auto c = [](float x) -> int8_t { return (int8_t)(glm::clamp(x, -1.0f, 1.0f) * 127.0f); };
    return glm::i8vec4(c(v.x), c(v.y), c(v.z), (int8_t)(glm::clamp(w, -1.0f, 1.0f) * 127.0f));
}
inline glm::vec3 UnpackSnorm3(const glm::i8vec4& v) {
    return glm::vec3((float)v.x, (float)v.y, (float)v.z) * (1.0f / 127.0f);
}
// HALF float 编码：vec2 → u16×2（x→低16位, y→高16位）
inline glm::u16vec2 PackHalf2(const glm::vec2& v) {
    const uint32_t h = glm::packHalf2x16(v);
    return glm::u16vec2((uint16_t)(h & 0xFFFFu), (uint16_t)((h >> 16) & 0xFFFFu));
}
inline glm::vec2 UnpackHalf2(const glm::u16vec2& v) {
    return glm::unpackHalf2x16((uint32_t)v.x | ((uint32_t)v.y << 16));
}

// ===== 静态模型压缩顶点（无骨骼，24B）=====
// 2026-08-17 已清理：StaticVertex/static_model.vert/静态 descriptor layout 全部移除（统一蒙皮 Vertex 32B 渲染），
// 仅存档结构定义以备将来（若真机验证静态专化值得，再重建并接入 model.vert 分支）。
// struct MIKAN_API StaticVertex { ... }（已删）

inline constexpr int MAX_BONES = 128; // 蒙皮矩阵上限（shader UBO 与渲染器共用）；128 骨骼=8KB UBO，低于 Vulkan 16KB 规范下限，全设备合法

struct MIKAN_API Bone {
    std::string name;
    int parentIndex = -1;
    glm::mat4 offsetMatrix;
    glm::mat4 bindMatrix;
    glm::mat4 localTransform;
    glm::mat4 bindLocalTransform;      // 绑定姿势局部变换（场景节点树变换，含根节点 Z_UP 等轴修正）
    glm::mat4 ancestorTransform;       // 非骨骼祖先节点累计变换（如 glTF 的 Z_UP/Armature 轴修正）
    glm::mat4 globalTransform;
    glm::vec3 initialPosition;
    glm::quat initialRotation;
    glm::vec3 position;
    glm::quat rotation;
};

// ===== 骨骼动画（assimp aiAnimation 归一化；glTF/FBX 通用）=====
struct MIKAN_API BoneKeyframe {
    float time = 0.0f;                       // 秒（时间轴 = rotation 通道优先，动画通常 rotation 主导）
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
};
struct MIKAN_API BoneChannel {
    int boneIndex = -1;                      // 对应 MeshData::bones 索引
    std::vector<BoneKeyframe> keyframes;     // 已按时间升序（统一时间轴）
};
struct MIKAN_API AnimationClip {
    std::string name;
    float duration = 0.0f;                   // 秒
    std::vector<BoneChannel> channels;
};

struct MIKAN_API SubMesh {
    std::string name;                 // 2026-08-09 subMesh 标识（assimp mesh 名，空则 "SubMesh_<n>"；per-subMesh 材质选择用）
    std::vector<Vertex> vertices;             // 蒙皮顶点（统一布局 32B；2026-08-17 静态 24B 路径已清理）
    std::vector<unsigned int> indices;
    std::string materialName;
    float metallic = -1.0f;    // 2026-08-11 glTF metallicFactor（-1=未设——用引擎默认）
    float roughness = -1.0f;   // 2026-08-11 glTF roughnessFactor（-1=未设——用引擎默认）
    int alphaMode = -1;        // 2026-08-16 glTF alphaMode：-1=未知(assimp 路径→shader 旧行为) 0=OPAQUE 1=MASK 2=BLEND
    float alphaCutoff = 0.5f;  // 2026-08-16 glTF alphaCutoff（仅 MASK 用，glTF 规范默认 0.5）
    bool doubleSided = false;  // 2026-08-16 glTF doubleSided（渲染接入留后续批次）
    float diffuseTransmissionFactor = 0.0f;  // 2026-08-29 KHR_materials_diffuse_transmission；未声明=0
    int hasMRTexture = 0;      // 2026-08-17 加载阶段标记：材质是否有 roughness/metallic 纹理引用（0=无→shader 回退 CPU 参数）。
                               // 按数据缺失判定，不判像素内容——黑色金属素材的 MR 纹理同样合法（黑≠无效）
    int materialIndex = -1;      // 2026-08-17 assimp 材质索引（mesh->mMaterialIndex，加载期记录——hasMRTexture 回填按索引映射，不靠名字匹配）
    int gltfMatIndex = -1;       // 2026-08-17 glTF 材质索引（subMesh 循环中已精确算出；doubleSided/alphaMode 回填用它——不依赖 assimpToGltfMat/材质名）
    int srcMesh = -1;            // 2026-08-17 来源 mesh 序号（assimp mesh 顺序 = gltf mesh 顺序）——doubleSided 回填主通道
};

struct MIKAN_API MaterialTextureInfo {
    std::string materialName;
    std::string diffuseTexturePath;
    std::string normalTexturePath;
    std::string specularTexturePath;
    std::string roughnessTexturePath;
    std::string metallicTexturePath;
    std::string emissiveTexturePath;   // 2026-08-09 自发光贴图（Bistro 发光体材质：BaseColor 黑 + *_Emissive.dds 发光）
    int wrapMode = 10497;   // 纹理环绕（VkSamplerAddressMode 值：10497=REPEAT / 33071=CLAMP_TO_EDGE / 33648=MIRRORED_REPEAT；来自 gltf sampler，per-texture）
    bool hasTexture;
    bool hasNormalTexture;
    bool hasSpecularTexture;    bool hasRoughnessTexture;
    bool hasMetallicTexture;
    bool hasEmissiveTexture;   // 2026-08-09
    glm::vec4 diffuse;
    glm::vec3 specular;
    float specularPower;
    glm::vec3 ambient;
    float metallic = -1.0f;    // 2026-08-11 glTF metallicFactor（-1=未设）
    float roughness = -1.0f;   // 2026-08-11 glTF roughnessFactor（-1=未设）
    int alphaMode = -1;        // 2026-08-16 glTF alphaMode：-1=未知(assimp 路径) 0=OPAQUE 1=MASK 2=BLEND
    float alphaCutoff = 0.5f;  // 2026-08-16 glTF alphaCutoff（仅 MASK 用）
    bool doubleSided = false;  // 2026-08-16 glTF doubleSided
    float diffuseTransmissionFactor = 0.0f;  // 2026-08-29 KHR_materials_diffuse_transmission；未声明=0
};

struct MIKAN_API MeshData {
    std::vector<SubMesh> subMeshes;
    std::vector<Bone> bones;
    std::vector<AnimationClip> animations;
    std::vector<MaterialTextureInfo> materialTextures;
    
    std::vector<glm::vec3> originalPositions;
    std::vector<glm::vec3> originalNormals;
    std::vector<glm::vec3> originalTangents;
    std::vector<glm::vec3> originalBitangents;
    
    bool isSceneModel = false;
};

struct MIKAN_API ModelLoadResult {
    MeshData meshData;
    std::vector<MaterialTextureInfo> materialTextures;
};

#ifdef __ANDROID__
class MIKAN_API AndroidIOStream : public Assimp::IOStream {
public:
    AndroidIOStream(const std::string& path);
    ~AndroidIOStream() override;
    
    size_t Read(void* pvBuffer, size_t pSize, size_t pCount) override;
    size_t Write(const void* pvBuffer, size_t pSize, size_t pCount) override;
    aiReturn Seek(size_t pOffset, aiOrigin pOrigin) override;
    size_t Tell() const override;
    size_t FileSize() const override;
    void Flush() override;
    
private:
    SDL_IOStream* m_IO;
    size_t m_Size;
};

class MIKAN_API AndroidIOSystem : public Assimp::IOSystem {
public:
    AndroidIOSystem() = default;
    ~AndroidIOSystem() override = default;
    
    bool Exists(const char* pFile) const override;
    char getOsSeparator() const override;
    Assimp::IOStream* Open(const char* pFile, const char* pMode) override;
    void Close(Assimp::IOStream* pFile) override;
};
#endif

class MIKAN_API ModelLoader {
public:
    static MeshData LoadModel(const std::string& path);
    static ModelLoadResult LoadModelWithTextures(const std::string& path);
    static void ClearCache();

    // 采样动画到骨骼：给定 clip 时间（秒，循环由调用方处理），更新 bones 的
    // localTransform/globalTransform/position/rotation。未在动画通道中的骨骼保持绑定姿势。
    // 蒙皮矩阵 = globalTransform * offsetMatrix（调用方每帧自行计算）。
    static bool SampleAnimation(const AnimationClip& clip, float time, std::vector<Bone>& bones);
    
private:
    static std::unordered_map<std::string, ModelLoadResult> s_ModelCache;
    static std::mutex s_CacheMutex;
};

#endif
