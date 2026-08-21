#version 450
// 高斯纵向 pass（σ=4，±8 tap，用横向 threshold 结果）——2026-08-11
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

const float SIGMA = 4.0;
const float INV_2SIGMA2 = 1.0 / (2.0 * SIGMA * SIGMA);

void main() {
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    vec3 c = vec3(0.0);
    float wsum = 0.0;
    for (int j = -8; j <= 8; j++) {
        float w = exp(-float(j) * float(j) * INV_2SIGMA2);
        c += texture(inputTex, fragTexCoord + vec2(0.0, texel.y * float(j))).rgb * w;
        wsum += w;
    }
    fragColor = vec4(c / wsum, 1.0);
}
