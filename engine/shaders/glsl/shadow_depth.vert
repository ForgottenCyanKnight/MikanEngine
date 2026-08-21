#version 450

// 点光源阴影顶点着色器（2026-08-13，蒙皮版）——zprepass.vert 的阴影变体：
// 输出光源空间 gl_Position（投影深度测试）+ 世界位置（fragment 算线性深度 dist/range）
// push constant 与 zprepass 兼容（projView @0；lightPosRange @64 供 fragment 用）

// 为 Adreno GPU 强制使用高精度
precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    vec4 lightPosRange;   // xyz = 光源世界位置，w = range
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;
layout(location = 16) in uvec4 inBoneIDs;
layout(location = 17) in vec4 inBoneWeights;

layout(location = 5) in mat4 inModel;
layout(location = 9) in mat4 inPrevModel;
layout(location = 13) in vec4 inAlbedoColor;
layout(location = 14) in vec4 inMaterialData;
layout(location = 15) in vec4 inTextureFlags;

layout(location = 0) out vec3 vWorldPos;

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
    vWorldPos = worldPos.xyz;
}
