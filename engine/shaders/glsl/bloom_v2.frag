#version 450
// FMDS BLOOM_3 精确移植（2026-08-11）：5-tap 垂直高斯（weights/offsets 照抄 FMDS）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // bloom_h2 输出

void main() {
    vec2 uv = fragTexCoord;
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    float weights[5];
    weights[0] = 0.19638062; weights[1] = 0.29675293; weights[2] = 0.09442139;
    weights[3] = 0.01037598; weights[4] = 0.00025940;
    float offsets[5];
    offsets[0] = 0.0; offsets[1] = 1.41176471; offsets[2] = 3.29411765;
    offsets[3] = 5.17647059; offsets[4] = 7.05882353;

    vec3 color = texture(inputTex, uv).rgb * weights[0];
    float weightSum = weights[0];
    for (int i = 1; i < 5; i++) {
        vec2 offset = offsets[i] * texel;
        color += texture(inputTex, uv + offset * vec2(0.0, 1.0)).rgb * weights[i];
        color += texture(inputTex, uv - offset * vec2(0.0, 1.0)).rgb * weights[i];
        weightSum += weights[i] * 2.0;
    }
    fragColor = vec4(color / weightSum, 1.0);
}
