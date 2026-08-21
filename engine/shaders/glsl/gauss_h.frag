#version 450
// 高斯横向 pass（σ=4，±8 tap）——2026-08-11 替代 boxblur（柔和衰减适合 bloom 大半径）
// threshold 提取 HDR 亮部（只模糊高光）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

#define BLOOM_THRESHOLD 1.0   // 线性 HDR 亮部阈值（>1 的高光）
const float SIGMA = 4.0;
const float INV_2SIGMA2 = 1.0 / (2.0 * SIGMA * SIGMA);

void main() {
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    vec3 c = vec3(0.0);
    float wsum = 0.0;
    for (int i = -8; i <= 8; i++) {
        float w = exp(-float(i) * float(i) * INV_2SIGMA2);
        c += texture(inputTex, fragTexCoord + vec2(texel.x * float(i), 0.0)).rgb * w;   // 全屏模糊（无 threshold——朦胧感）
        wsum += w;
    }
    fragColor = vec4(c / wsum, 1.0);
}
