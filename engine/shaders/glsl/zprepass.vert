#version 450

// z-prepass 顶点着色器（subpass 0，depth-only）——与 model.vert 相同的蒙皮+顶点变换，仅输出 gl_Position
// 目的：提前写深度，MRT 几何阶段（subpass 1）被遮挡片元在 fragment shader 前被深度测试剔除，
// 减少 G-Buffer 多附件写入的 overdraw（几何 shader 含 discard 关键字禁用 early-z，z-prepass 无 discard 可早期剔除）

// 为 Adreno GPU 强制使用高精度
precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    mat4 prevProjView;
    vec3 cameraPosition;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inNormal;      // 压缩：SNORM int8 → 驱动自动归一化 -1..1
layout(location = 2) in vec2 inTexCoord;    // 压缩：HALF float
layout(location = 3) in vec4 inTangent;     // 压缩：xyz=切线方向(SNORM) + w=±1 手性
layout(location = 16) in uvec4 inBoneIDs;   // 压缩：UINT8 ×4
layout(location = 17) in vec4 inBoneWeights;// 压缩：UNORM ×4 → 驱动自动归一化 0..1

layout(location = 5) in mat4 inModel;
layout(location = 9) in mat4 inPrevModel;
layout(location = 13) in vec4 inAlbedoColor;
layout(location = 14) in vec4 inMaterialData;
layout(location = 15) in vec4 inTextureFlags;

// alpha test 需要：UV + textureFlags（model_zprepass.frag 采样 albedo 做 discard）
layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragTextureFlags;

// 骨骼蒙皮矩阵（与 model.vert 一致：UBO 固定 128）
#define MAX_BONES 128
layout(binding = 4) uniform BoneMatricesUBO { mat4 bones[MAX_BONES]; } boneData;

void main() {
    vec3 skinPos = inPosition;
    float totalWeight = inBoneWeights.x + inBoneWeights.y + inBoneWeights.z + inBoneWeights.w;
    if (totalWeight > 0.001) {
        ivec4 skinIds = clamp(ivec4(inBoneIDs), ivec4(0), ivec4(MAX_BONES - 1));
        mat4 skinMat = mat4(0.0);
        if (inBoneWeights.x > 0.0) skinMat += inBoneWeights.x * boneData.bones[skinIds.x];
        if (inBoneWeights.y > 0.0) skinMat += inBoneWeights.y * boneData.bones[skinIds.y];
        if (inBoneWeights.z > 0.0) skinMat += inBoneWeights.z * boneData.bones[skinIds.z];
        if (inBoneWeights.w > 0.0) skinMat += inBoneWeights.w * boneData.bones[skinIds.w];
        skinPos = (skinMat * vec4(inPosition, 1.0)).xyz;
    }

    vec4 worldPos = inModel * vec4(skinPos, 1.0);
    gl_Position = pc.projView * worldPos;

    // alpha test 需要：UV + textureFlags（model_zprepass.frag 采样 albedo 做 discard）
    fragTexCoord = inTexCoord;
    fragTextureFlags = inTextureFlags;
}
