#version 450

// 2D Canvas 顶点着色器：顶点已由 CPU 变换到世界/屏幕坐标，这里只乘正交投影
layout(push_constant) uniform PushConstants {
    mat4 viewProj;   // 正交投影 * 视图（16 float = 64B，顶点阶段）
} pc;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inScreenPxRange;   // 距离场过渡带屏幕像素（位图模式=0 忽略）

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out float fragScreenPxRange;

void main() {
    vec4 clip = pc.viewProj * vec4(inPos, 0.0, 1.0);
    clip.z = 0.0;   // 强制 NDC z=0 → 深度恒 0：2D 世界层识别标记（合成 shader depth<=0.0001 直通，不做 3D 光照）；
                    // 不经过 z 测试（depthTest=false，layer 驱动前后），写深度 0 仅为合成区分 2D/3D/天空
    gl_Position = clip;
    fragUV = inUV;
    fragColor = inColor;
    fragScreenPxRange = inScreenPxRange;
}
