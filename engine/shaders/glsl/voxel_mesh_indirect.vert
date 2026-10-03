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

layout(location = 3) in uint sourceInstance;
layout(binding = 0) uniform samplerBuffer sourceData;
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
    int base = int(sourceInstance) * 12;
    mat4 aModel = mat4(texelFetch(sourceData, base), texelFetch(sourceData, base + 1),
        texelFetch(sourceData, base + 2), texelFetch(sourceData, base + 3));
    mat4 aPrevModel = mat4(texelFetch(sourceData, base + 4), texelFetch(sourceData, base + 5),
        texelFetch(sourceData, base + 6), texelFetch(sourceData, base + 7));
    vec4 aAlbedoColor = texelFetch(sourceData, base + 8);
    vec4 minSize = texelFetch(sourceData, base + 10);
    vec3 aWorldMinBounds = minSize.xyz;
    float aVoxelSize = minSize.w;
    // 从 16 bits 中提取面方向和材质索引
    // 位分配：[0-2] 面方向 (3 bits), [3-15] 材质索引 (13 bits)
    int faceDir = int(aFaceDirAndMaterial & uint(0x07));
    uint materialIndex = (aFaceDirAndMaterial >> 3) & uint(0x1FFF);
    
    // 使用面方向计算法线
    fragNormal = normalize(transpose(inverse(mat3(aModel))) * GetFaceNormal(faceDir));
    
    // 使用顶点颜色（取前 3 个分量）
    fragAlbedoColor = vec4(vec3(aColor.rgb) / 255.0, 1.0) * aAlbedoColor;
    
    // 使用硬编码材质或从材质索引获取
    // 这里暂时使用默认材质
    fragMaterialData = vec4(0.0, 1.0, 1.0, 0.0);
    
    vec3 relativePos = vec3(aPos.xyz) * aVoxelSize + aWorldMinBounds;
    vec4 worldPos = aModel * vec4(relativePos, 1.0);
    
    gl_Position = pc.projView * worldPos;
    vec4 previousClip = pc.prevProjView * aPrevModel * vec4(relativePos, 1.0);
    fragMotionVector = (gl_Position.w > 0.0001 && previousClip.w > 0.0001)
        ? (gl_Position.xy / gl_Position.w - previousClip.xy / previousClip.w) * 0.5
        : vec2(0.0);
    fragPosition = worldPos.xyz;
}
