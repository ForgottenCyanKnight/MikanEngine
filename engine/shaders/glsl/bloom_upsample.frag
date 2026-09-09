#version 450

// binding 0 = 更小的 bloom（双线性放大 = 模糊）、binding 1 = 大 bloom 或原有颜色附件（composite）
// 输出 = 两者相加（bloom 逐级累加回全分辨率——最后一级的 input1 是 composite——bloom 混合到原图上）

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D inputTex;    // 小 bloom（upsample——双线性放大即模糊）
layout(binding = 1) uniform sampler2D inputTex2;   // 大 bloom / composite（add——原有颜色附件）

void main() {
    vec3 a = texture(inputTex, fragTexCoord).rgb;
    vec3 b = texture(inputTex2, fragTexCoord).rgb;
    outColor = vec4(a + b, 1.0);
}
