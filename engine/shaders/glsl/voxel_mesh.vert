#version 450

precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    mat4 prevProjView;
    vec3 cameraPosition;
    float padding;
} pc;

layout(location = 0) in uvec4 aPos;
layout(location = 1) in uvec4 aColor;
layout(location = 2) in uint aFaceDirAndMaterial;

layout(location = 3) in mat4 aModel;
layout(location = 7) in mat4 aPrevModel;
layout(location = 11) in vec4 aAlbedoColor;
layout(location = 12) in vec4 aMaterialData;
layout(location = 13) in vec3 aWorldMinBounds;
layout(location = 14) in float aVoxelSize;

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragAlbedoColor;
layout(location = 3) out vec4 fragMaterialData;
layout(location = 4) out vec2 fragMotionVector;

vec3 GetFaceNormal(int faceDir) {
    switch(faceDir) {
        case 0: return vec3(0.0, 0.0, 1.0);
        case 1: return vec3(0.0, 0.0, -1.0);
        case 2: return vec3(-1.0, 0.0, 0.0);
        case 3: return vec3(1.0, 0.0, 0.0);
        case 4: return vec3(0.0, 1.0, 0.0);
        case 5: return vec3(0.0, -1.0, 0.0);
        default: return vec3(0.0, 0.0, 1.0);
    }
}

void main() {
    // 从 16 bits 中提取面方向和材质索引
    // 位分配：[0-2] 面方向 (3 bits), [3-15] 材质索引 (13 bits)
    int faceDir = int(aFaceDirAndMaterial & uint(0x07));
    uint materialIndex = (aFaceDirAndMaterial >> 3) & uint(0x1FFF);
    
    // 使用面方向计算法线
    fragNormal = GetFaceNormal(faceDir);
    
    // 使用顶点颜色（取前 3 个分量）
    fragAlbedoColor = vec4(vec3(aColor.rgb) / 255.0, 1.0);
    
    // 使用硬编码材质或从材质索引获取
    // 这里暂时使用默认材质
    fragMaterialData = vec4(0.5, 0.5, 1.0, 0.0);
    
    fragMotionVector = vec2(0.5);
    
    vec3 relativePos = vec3(aPos.xyz) * aVoxelSize + aWorldMinBounds;
    vec4 worldPos = aModel * vec4(relativePos, 1.0);
    
    gl_Position = pc.projView * worldPos;
    fragPosition = worldPos.xyz;
}
