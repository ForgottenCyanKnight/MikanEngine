#version 450
// 全图采样 + 阈值提取（多尺度由下游真实金字塔提供）
#define BLOOM_THRESHOLD 0.4
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // composite（全尺寸线性 HDR）

void main() {
    vec3 color = texture(inputTex, fragTexCoord).rgb;   // 全图归一化采样（CutTex 区域拼贴已移除）
    //color = max(color - vec3(BLOOM_THRESHOLD), 0.0);    // 亮部提取（soft knee 简化：硬阈值）
    fragColor = vec4(color, 1.0);
}
