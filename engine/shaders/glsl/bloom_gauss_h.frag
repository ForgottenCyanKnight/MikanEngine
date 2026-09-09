#version 450

// 双线性采样配合奇数半偏移（1.41/3.29/5.18/7.06 texel）——每偏移等效 2 采样

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D inputTex;

void main() {
    const float weights[5] = { 0.19638062, 0.29675293, 0.09442139, 0.01037598, 0.00025940 };
    const float offsets[5] = { 0.0, 1.41176471, 3.29411765, 5.17647059, 7.05882353 };
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    vec3 c = texture(inputTex, fragTexCoord).rgb * weights[0];
    float wsum = weights[0];
    for (int i = 1; i < 5; i++) {
        vec2 off = vec2(offsets[i] * texel.x, 0.0);
        c += texture(inputTex, fragTexCoord + off).rgb * weights[i];
        c += texture(inputTex, fragTexCoord - off).rgb * weights[i];
        wsum += 2.0 * weights[i];
    }
    outColor = vec4(c / wsum, 1.0);
}
