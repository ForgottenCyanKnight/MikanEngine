#version 450
// 层叠布局——输出 [0,2]×[0,1] 逻辑空间（mikan 附件 x∈[0,1] 映射 ×2）——
// 每像素累加 5 个 octave 的 samplebloom（区段由出界判断自动产生：octave n 有效 x ∈ [off, off+1/scale]）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // composite

const float LOG2E = 1.4426950408889634;

vec2 caloffset(float octave) { return vec2(-(1.0 - 1.0 / exp2(octave)), 0.0); }

// FMDS gauss1Dx：水平高斯循环（双线性半偏移 2i+0.5）——texelSize = [0,2] 逻辑空间（2/W, 1/H）
vec3 gauss1Dx(vec2 coord, float alpha, int maxIT) {
    vec2 texelSize = vec2(2.0, 1.0) / vec2(textureSize(inputTex, 0));
    vec4 tot = vec4(0.0);
    for (int i = -maxIT; i <= maxIT; i++) {
        float weight = exp2(-float(i) * float(i) * alpha * 5.77 * LOG2E);
        vec2 spCoord = coord + vec2(1.0, 0.0) * texelSize * (2.0 * float(i) + 0.5);
        tot += vec4(texture(inputTex, spCoord).rgb, 1.0) * weight;
    }
    return tot.rgb / max(1.0, tot.a);
}

// FMDS BLOOM_0 samplebloom：scale = exp2(octave - 1.0)
vec3 samplebloom(vec2 uv, float octave, vec2 off, float a, int m) {
    float scale = exp2(octave - 1.0);
    uv += off;
    uv.x *= scale;
    if (uv.x < 0.0 || uv.x > 1.0) return vec3(0.0);
    return gauss1Dx(uv, a, m);
}

void main() {
    vec2 uv = vec2(fragTexCoord.x * 2.0, fragTexCoord.y);   // [0,2]×[0,1] 逻辑空间
    vec3 col = vec3(0.0);
    col += samplebloom(uv, 1.0, vec2(0.0), 0.16, 0);        // octave 1
    col += samplebloom(uv, 2.0, caloffset(1.0), 0.16, 3);   // octave 2
    col += samplebloom(uv, 3.0, caloffset(2.0), 0.035, 6);  // octave 3
    col += samplebloom(uv, 4.0, caloffset(3.0), 0.0085, 12);// octave 4
    col += samplebloom(uv, 5.0, caloffset(4.0), 0.002, 30); // octave 5
    fragColor = vec4(col, 1.0);
}
