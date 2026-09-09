#version 450

// 用法：降采样 pass（scale=0.5/0.25），输出亮部供后续 Kawase/双线性上采样模糊

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec3 c = texture(inputTex, fragTexCoord).rgb;
    const float threshold = 1.0;   // 亮度阈值（>1 线性 HDR 才 bloom——可调）
    const float knee = 0.5;        // soft knee 过渡宽度（避免硬切——平滑亮部边缘）
    // 标准 soft knee：低于 threshold 为 0、knee 范围内平滑过渡、之上线性
    vec3 kneeVec = max(c - threshold + knee, vec3(0.0));
    vec3 k = kneeVec * kneeVec / (4.0 * knee + 1e-5);
    vec3 bright = max(c - threshold, vec3(0.0)) + k;
    outColor = vec4(bright, 1.0);
}
