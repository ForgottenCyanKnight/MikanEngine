#version 450

// 点光源阴影顶点着色器（2026-08-13，静态模型版）——zprepass_static.vert 的阴影变体：
// 输出光源空间 gl_Position + 世界位置（fragment 算线性深度 dist/range）
layout(push_constant) uniform PushConstants {
    mat4 projView;
    vec4 lightPosRange;   // xyz = 光源世界位置，w = range
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 5) in mat4 inModel;

layout(location = 0) out vec3 vWorldPos;

void main() {
    vec4 worldPos = inModel * vec4(inPosition, 1.0);
    gl_Position = pc.projView * worldPos;
    vWorldPos = worldPos.xyz;
}
