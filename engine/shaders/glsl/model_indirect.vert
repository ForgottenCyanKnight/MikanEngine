#version 450

// 为 Adreno GPU 强制使用高精度
precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    mat4 prevProjView;
    vec3 cameraPosition;
    vec2 taaJitter;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inNormal;      // 压缩：SNORM int8 → 驱动自动归一化 -1..1
layout(location = 2) in vec2 inTexCoord;    // 压缩：HALF float
layout(location = 3) in vec4 inTangent;     // 压缩：xyz=切线方向(SNORM) + w=±1 手性（Bitangent 不再存储，推导）










layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out highp vec2 fragTexCoord;
layout(location = 3) out vec3 fragTangent;
layout(location = 4) out vec3 fragBitangent;
layout(location = 5) out vec4 fragAlbedoColor;
layout(location = 6) out vec4 fragMaterialData;
layout(location = 7) out vec4 fragTextureFlags;
layout(location = 8) out vec2 fragMotionVector;

layout(location = 5) in uint sourceIndex;
layout(set = 1, binding = 0) uniform samplerBuffer instanceSource;
void main() {
    int base = int(sourceIndex) * 14;
    mat4 inModel = mat4(texelFetch(instanceSource, base), texelFetch(instanceSource, base+1), texelFetch(instanceSource, base+2), texelFetch(instanceSource, base+3));
    mat4 inPrevModel = mat4(texelFetch(instanceSource, base+4), texelFetch(instanceSource, base+5), texelFetch(instanceSource, base+6), texelFetch(instanceSource, base+7));
    vec4 inAlbedoColor = texelFetch(instanceSource, base+8);
    vec4 inMaterialData = texelFetch(instanceSource, base+9);
    vec4 inTextureFlags = texelFetch(instanceSource, base+10);
    // ===== 骨骼蒙皮（GPU 主路径：顶点着色器读 UBO；CPU 蒙皮 fallback 时顶点已预变换，weight 全 0 直通）=====
    vec3 skinPos = inPosition;
    // 解码压缩法线/切线（SNORM 驱动已归一化到 -1..1，再 normalize 稳）+ 推导 bitangent（不再存储）
    vec3 skinNormal = normalize(inNormal.xyz);
    vec3 skinTangent = normalize(inTangent.xyz);
    vec3 skinBitangent = normalize(cross(skinNormal, skinTangent)) * (inTangent.w >= 0.0 ? 1.0 : -1.0);
    // 当前帧世界位置
    vec4 worldPos = inModel * vec4(skinPos, 1.0);
    gl_Position = pc.projView * worldPos;
    gl_Position.xy += pc.taaJitter * gl_Position.w;
    fragPosition = worldPos.xyz;
    
    // 上一帧的裁剪空间位置（使用上一帧的 ProjView 和 PrevModelMatrix——均无 jitter，motion 纯运动量）
    vec4 prevWorldPos = inPrevModel * vec4(skinPos, 1.0);
    vec4 prevClipPos = pc.prevProjView * prevWorldPos;
    
    // Compute a UV-space motion vector after removing current-frame jitter.
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
