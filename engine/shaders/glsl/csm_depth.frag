#version 450

// 方向光 CSM 阴影深度片元（2026-08-14）：保留默认 NDC 深度
// 正交投影下 NDC z 线性，采样端直接比较 clipPos.z 映射后的深度纹理值（+bias），无需线性化
// push constant 与 shadow_depth.vert 兼容（80B：projView + lightPosRange——frag 忽略 lightPosRange）
// 顶点着色器复用 shadow_depth.vert（蒙皮版）/ shadow_depth_static.vert（静态版）
precision highp float;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    vec4 lightPosRange;   // 未使用（CSM 无需线性深度）
} pc;

void main() {
    // 默认 gl_FragDepth（NDC z 经 viewport 映射 [0,1]）
}
