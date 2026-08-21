#version 450

precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 view;
    mat4 proj;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inNormal;      // 压缩：SNORM int8 → 驱动归一化 -1..1
layout(location = 2) in vec2 inTexCoord;    // 压缩：HALF
layout(location = 3) in vec4 inTangent;     // 压缩：xyz 方向 + w 手性（Bitangent 不再存储，推导）

layout(location = 5) in mat4 inModel;
layout(location = 9) in vec4 inAlbedoColor;
layout(location = 10) in vec4 inMaterialData;
layout(location = 11) in vec4 inTextureFlags;

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out highp vec2 fragTexCoord;
layout(location = 3) out vec3 fragTangent;
layout(location = 4) out vec3 fragBitangent;
layout(location = 5) out vec4 fragAlbedoColor;
layout(location = 6) out vec4 fragMaterialData;
layout(location = 7) out vec4 fragTextureFlags;

void main() {
    gl_Position = pc.proj * pc.view * inModel * vec4(inPosition, 1.0);
    fragPosition = vec3(inModel * vec4(inPosition, 1.0));
    
    mat3 normalMatrix = transpose(inverse(mat3(inModel)));
    // 解包压缩法线/切线 + 推导 bitangent（不再存储）
    vec3 normalV = normalize(inNormal.xyz);
    vec3 tangentV = normalize(inTangent.xyz);
    vec3 bitangentV = normalize(cross(normalV, tangentV)) * (inTangent.w >= 0.0 ? 1.0 : -1.0);
    fragNormal = normalize(normalMatrix * normalV);
    fragTangent = normalize(normalMatrix * tangentV);
    fragBitangent = normalize(normalMatrix * bitangentV);
    
    fragTexCoord = inTexCoord;
    fragAlbedoColor = inAlbedoColor;
    fragMaterialData = inMaterialData;
    fragTextureFlags = inTextureFlags;
}
