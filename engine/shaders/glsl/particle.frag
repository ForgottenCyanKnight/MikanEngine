#version 450

layout(location = 0) in vec2 outUv;
layout(location = 1) in vec4 outColor;
layout(location = 0) out vec4 fragColor;

void main() {
    // 第一阶段不强制引入纹理资产，使用软圆形 alpha 作为默认粒子材质。
    // 后续加入 atlas 时只需把这里替换为 sampler2D 采样，不改变实例流。
    const float distanceToCenter = length(outUv * 2.0 - 1.0);
    const float softMask = 1.0 - smoothstep(0.72, 1.0, distanceToCenter);
    const float alpha = outColor.a * softMask;
    if (alpha <= 0.001) discard;
    fragColor = vec4(outColor.rgb, alpha);
}
