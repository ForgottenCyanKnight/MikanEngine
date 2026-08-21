#version 450
// FMDS BLOOM_1 精确移植（2026-08-11）：
// ⚠️ scale = exp2(octave)（非 octave-1！）、uv.xy *= scale（xy 都乘）、gauss1Dy 的 coord.y = uv.y*scale*0.5、texelSize.y = 0.5/H
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // bloom_h 输出

const float LOG2E = 1.4426950408889634;

vec2 caloffset(float octave) { return vec2(-(1.0 - 1.0 / exp2(octave)), 0.0); }

vec3 gauss1Dy(vec2 coord, float alpha, int maxIT) {
    vec2 texelSize = vec2(2.0, 1.0) / vec2(textureSize(inputTex, 0));   // [0,2] 逻辑空间 texel（与 bloom_h 一致）
    vec4 tot = vec4(0.0);
    for (int i = -maxIT; i <= maxIT; i++) {
        float weight = exp2(-float(i) * float(i) * alpha * 5.77 * LOG2E);
        vec2 spCoord = coord + vec2(0.0, 1.0) * texelSize * (2.0 * float(i) + 0.5);
        tot += vec4(texture(inputTex, spCoord).rgb, 1.0) * weight;
    }
    return tot.rgb / max(1.0, tot.a);
}

vec3 samplebloom(vec2 uv, float octave, vec2 off, float a, int m) {
    float scale = exp2(octave - 1.0);   // 与 bloom_h 一致（自洽布局）
    uv += off;
    uv.x *= scale;
    if (uv.x < 0.0 || uv.x > 1.0) return vec3(0.0);
    return gauss1Dy(uv, a, m);   // y 全高（不做 FMDS 的 y 打包——mikan 每 pass 全屏附件）
}

void main() {
    vec2 uv = vec2(fragTexCoord.x * 2.0, fragTexCoord.y);
    vec3 col = vec3(0.0);
    col += samplebloom(uv, 1.0, vec2(0.0), 0.16, 0);
    col += samplebloom(uv, 2.0, caloffset(1.0), 0.16, 3);
    col += samplebloom(uv, 3.0, caloffset(2.0), 0.035, 6);
    col += samplebloom(uv, 4.0, caloffset(3.0), 0.0085, 12);
    col += samplebloom(uv, 5.0, caloffset(4.0), 0.002, 30);
    fragColor = vec4(col, 1.0);
}
