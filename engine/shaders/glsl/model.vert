#version 450

// 为 Adreno GPU 强制使用高精度
precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    mat4 prevProjView;
    vec3 cameraPosition;
    vec2 taaJitter;   // 2026-08-17：TAA 亚像素抖动（NDC 偏移；TAA 禁用时 = 0）
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inNormal;      // 压缩：SNORM int8 → 驱动自动归一化 -1..1
layout(location = 2) in vec2 inTexCoord;    // 压缩：HALF float
layout(location = 3) in vec4 inTangent;     // 压缩：xyz=切线方向(SNORM) + w=±1 手性（Bitangent 不再存储，推导）
layout(location = 16) in uvec4 inBoneIDs;   // 压缩：UINT8 ×4
layout(location = 17) in vec4 inBoneWeights;// 压缩：UNORM ×4 → 驱动自动归一化 0..1

layout(location = 5) in mat4 inModel;
layout(location = 9) in mat4 inPrevModel;
layout(location = 13) in vec4 inAlbedoColor;
layout(location = 14) in vec4 inMaterialData;
layout(location = 15) in vec4 inTextureFlags;

// 骨骼蒙皮矩阵（binding 4，UBO 固定 256；顶点着色器动态索引 UBO 数组在本机 NVIDIA 桌面/移动端均稳定；
// 2026-08-06 确认：早期 UBO 版 DEVICE_LOST 系悬垂指针 UB/未 clamp 越界叠加，修复后 UBO 稳定）
#define MAX_BONES 256
layout(binding = 4) uniform BoneMatricesUBO { mat4 bones[MAX_BONES]; } boneData;

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out highp vec2 fragTexCoord;
layout(location = 3) out vec3 fragTangent;
layout(location = 4) out vec3 fragBitangent;
layout(location = 5) out vec4 fragAlbedoColor;
layout(location = 6) out vec4 fragMaterialData;
layout(location = 7) out vec4 fragTextureFlags;
layout(location = 8) out vec2 fragMotionVector;

void main() {
    // ===== 骨骼蒙皮（GPU 主路径：顶点着色器读 UBO；CPU 蒙皮 fallback 时顶点已预变换，weight 全 0 直通）=====
    vec3 skinPos = inPosition;
    // 解码压缩法线/切线（SNORM 驱动已归一化到 -1..1，再 normalize 稳）+ 推导 bitangent（不再存储）
    vec3 skinNormal = normalize(inNormal.xyz);
    vec3 skinTangent = normalize(inTangent.xyz);
    vec3 skinBitangent = normalize(cross(skinNormal, skinTangent)) * (inTangent.w >= 0.0 ? 1.0 : -1.0);
    float totalWeight = inBoneWeights.x + inBoneWeights.y + inBoneWeights.z + inBoneWeights.w;
    if (totalWeight > 0.001) {
        // 蒙皮：UBO 数组按骨骼索引直接取矩阵（2026-08-06 由 texelFetch 改为 UBO——texel buffer 蒙皮实测不渲染）
        // boneIDs 钳制到 [0, MAX_BONES-1]：脏数据越界读 UBO → VK_ERROR_DEVICE_LOST（2026-08-06 实测 CesiumMan.glb）
        ivec4 skinIds = clamp(ivec4(inBoneIDs), ivec4(0), ivec4(MAX_BONES - 1));
        mat4 skinMat = mat4(0.0);
        if (inBoneWeights.x > 0.0) skinMat += inBoneWeights.x * boneData.bones[skinIds.x];
        if (inBoneWeights.y > 0.0) skinMat += inBoneWeights.y * boneData.bones[skinIds.y];
        if (inBoneWeights.z > 0.0) skinMat += inBoneWeights.z * boneData.bones[skinIds.z];
        if (inBoneWeights.w > 0.0) skinMat += inBoneWeights.w * boneData.bones[skinIds.w];
        skinPos = (skinMat * vec4(inPosition, 1.0)).xyz;
        skinNormal = normalize(mat3(skinMat) * skinNormal);
        skinTangent = normalize(mat3(skinMat) * skinTangent);
        skinBitangent = normalize(mat3(skinMat) * skinBitangent);
    }

    // 当前帧世界位置
    vec4 worldPos = inModel * vec4(skinPos, 1.0);
    gl_Position = pc.projView * worldPos;
    // 2026-08-17：TAA 亚像素抖动（IDKEngine 语义）——只平移光栅化位置（xy += jitter·w，z/w 不变深度一致）
    gl_Position.xy += pc.taaJitter * gl_Position.w;
    fragPosition = worldPos.xyz;
    
    // 上一帧的裁剪空间位置（使用上一帧的 ProjView 和 PrevModelMatrix——均无 jitter，motion 纯运动量）
    vec4 prevWorldPos = inPrevModel * vec4(skinPos, 1.0);
    vec4 prevClipPos = pc.prevProjView * prevWorldPos;
    
    // 计算运动矢量（IDKEngine 语义：无 +0.5 偏置，UV 空间差；当前 NDC 还原 jitter——几何真实位置）
    vec2 thisNdc = gl_Position.xy / gl_Position.w - pc.taaJitter;
    
    // 上一帧 NDC 坐标
    vec2 historyNdc = prevClipPos.xy / prevClipPos.w;
    
    // 运动矢量 = 当前帧 NDC - 上一帧 NDC，转换到 UV 空间 [0, 1]（×0.5——[−1,1]→[−0.5,0.5]，采样端 histUV = uv − motion）
    vec2 motionVector = vec2((thisNdc - historyNdc) * 0.5);

    fragMotionVector = motionVector;

    
    mat3 normalMatrix = transpose(inverse(mat3(inModel)));
    fragNormal = normalize(normalMatrix * skinNormal);
    fragTangent = normalize(normalMatrix * skinTangent);
    fragBitangent = normalize(normalMatrix * skinBitangent);
    
    fragTexCoord = inTexCoord;
    fragAlbedoColor = inAlbedoColor;
    fragMaterialData = inMaterialData;
    fragTextureFlags = inTextureFlags;
}
