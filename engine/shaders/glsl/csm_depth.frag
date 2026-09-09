#version 450

// 正交投影下 NDC z 线性，采样端直接比较 clipPos.z 映射后的深度纹理值（+bias），无需线性化
// push constant 与 shadow_depth.vert 兼容（96B：projView + lightPosRange + subMeshAlpha）
// The CSM pipeline reuses shadow_depth.vert for its vertex stage.
precision highp float;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    vec4 lightPosRange;   // 未使用（CSM 无需线性深度）
    vec4 subMeshAlpha;    // x = alphaCutoff，y = alphaMode（-1 unknown, 0 opaque, 1 mask, 2 blend）
} pc;

layout(location = 1) in vec2 vTexCoord;
layout(location = 2) flat in vec4 vTextureFlags;

layout(binding = 0) uniform sampler2D albedoTexture;

void main() {
    // CSM 深度必须与主模型/深度预通过滤同一套 alpha 语义，否则树叶贴图
    // 的透明区域会被当成实体写进级联阴影。
    float alphaMode = pc.subMeshAlpha.y;
    if (alphaMode > 1.5) {
        discard;
    }
    if (vTextureFlags.y > 0.5 && (alphaMode > 0.5 || alphaMode < -0.5)) {
        float cutoff = alphaMode > 0.5 ? pc.subMeshAlpha.x : 0.5;
        if (texture(albedoTexture, vTexCoord).a < cutoff) {
            discard;
        }
    }
    // 默认 gl_FragDepth（NDC z 经 viewport 映射 [0,1]）
}
