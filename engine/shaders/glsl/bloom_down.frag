#version 450
// 2026-08-12：demo 式 bloom 降采样（参考 D:\Shader project\WorkPlace\demo\shaders\glsl\bloom.fsh）
// 多尺度偏移采样（2.0/2.15/2.2/2.25 缩放区域）→ 半尺寸模糊图
// 无 threshold（全屏模糊——用户拍板"不要提取亮色，全屏模糊朦胧感"）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // composite（全尺寸线性 HDR）

vec4 CutTex(vec2 uv, vec2 offset, float fact) {
    vec2 newCoord = (uv - offset) * fact;
    if (newCoord.x < 0.0 || newCoord.x > 1.0 || newCoord.y < 0.0 || newCoord.y > 1.0) return vec4(0.0);
    return texture(inputTex, newCoord);
}

void main() {
    vec2 uv = fragTexCoord;
    vec3 color = vec3(0.0);
    color += CutTex(uv, vec2(0.0, 1.0 - 1.0/2.0), 2.0).rgb;
    color += CutTex(uv, vec2(1.0 - 1.0/2.15), 2.15).rgb;
    color += CutTex(uv, vec2(0.0), 2.2).rgb;
    color += CutTex(uv, vec2(1.0 - 1.0/2.25, 0.0), 2.25).rgb;
    fragColor = vec4(color, 1.0);   // 4 尺度累加（up 端归一化）
}
