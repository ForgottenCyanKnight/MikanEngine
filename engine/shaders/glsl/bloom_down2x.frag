#version 450
// Downsample kernel variants: default 3x3, dual, and Gaussian 5x5.
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

#ifdef BLOOM_KERNEL_DUAL
// Kawase dual 降采样核：中心×4 + 四对角(±1,±1)×1，÷8——有效半径 ~2 texel（扩散大）
vec3 Kernel(sampler2D tex, vec2 uv, vec2 px) {
    vec3 sum = texture(tex, uv).rgb * 4.0;
    sum += texture(tex, uv + vec2(-px.x,  px.y)).rgb;
    sum += texture(tex, uv + vec2( px.x,  px.y)).rgb;
    sum += texture(tex, uv + vec2(-px.x, -px.y)).rgb;
    sum += texture(tex, uv + vec2( px.x, -px.y)).rgb;
    return sum * 0.125;
}
#elif defined(BLOOM_KERNEL_GAUSS5)
// 高斯 5×5（sigma=1.0，标准归一化核）：中心 0.150、±1 正交 0.095、±1 对角 0.060、
// ±2 正交 0.024、±2/±1 混合 0.015、±2 对角 0.004——扩散 ±2 texel，能量归一 ≈1.0
const float K[25] = float[](
    0.003765, 0.015019, 0.023792, 0.015019, 0.003765,
    0.015019, 0.059912, 0.094907, 0.059912, 0.015019,
    0.023792, 0.094907, 0.150342, 0.094907, 0.023792,
    0.015019, 0.059912, 0.094907, 0.059912, 0.015019,
    0.003765, 0.015019, 0.023792, 0.015019, 0.003765
);
vec3 Kernel(sampler2D tex, vec2 uv, vec2 px) {
    vec3 sum = vec3(0.0);
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++)
            sum += texture(tex, uv + vec2(float(x), float(y)) * px).rgb
                 * K[(y + 2) * 5 + (x + 2)];
    return sum;
}
#else
// 高斯 3×3（sigma=1.0）：中心 0.195、正交邻 0.123、对角 0.078——扩散 ±1 texel，能量归一 ≈1.0
const float K[9] = float[](
    0.077847, 0.123317, 0.077847,
    0.123317, 0.195346, 0.123317,
    0.077847, 0.123317, 0.077847
);
vec3 Kernel(sampler2D tex, vec2 uv, vec2 px) {
    vec3 sum = vec3(0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            sum += texture(tex, uv + vec2(float(x), float(y)) * px).rgb
                 * K[(y + 1) * 3 + (x + 1)];
    return sum;
}
#endif

void main() {
    vec2 px = 1.0 / vec2(textureSize(inputTex, 0));
    fragColor = vec4(Kernel(inputTex, fragTexCoord, px), 1.0);
}
