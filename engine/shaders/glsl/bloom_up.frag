#version 450
// 2026-08-12：demo 式 bloom 合成（参考 demo bloomfinal.fsh）——多尺度加权采样 → 全尺寸 bloom 光晕
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // bloom_down 半尺寸模糊图

vec4 getScaleInverse(vec2 pos, vec2 offset, float fact) {
    return texture(inputTex, pos / fact + offset);
}

void main() {
    vec2 uv = fragTexCoord;
    vec3 bloom = vec3(0.0);
    bloom += getScaleInverse(uv, vec2(0.0, 1.0 - 1.0/2.0), 2.0).rgb * 3.0;
    bloom += getScaleInverse(uv, vec2(1.0 - 1.0/2.15), 2.15).rgb * 2.0;
    bloom += getScaleInverse(uv, vec2(0.0), 2.2).rgb * 1.5;
    bloom += getScaleInverse(uv, vec2(1.0 - 1.0/2.25, 0.0), 2.25).rgb * 1.0;
    fragColor = vec4(bloom / 7.5, 1.0);   // 归一化（权重和 3+2+1.5+1）
}
