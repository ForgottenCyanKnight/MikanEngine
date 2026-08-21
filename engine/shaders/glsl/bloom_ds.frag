#version 450
// 2026-08-13：3x3 高斯核降采样（sigma≈1.0）——替代 Kawase 8-tap
// Kawase 对角×2/轴向×1 各向异性（星形扩散），3 级迭代不足以均匀化；
// 高斯卷积封闭：多级级联仍是高斯（sigma 递增），各级均匀各向同性
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec2 uv = fragTexCoord;
    vec2 px = 1.0 / vec2(textureSize(inputTex, 0));   // texel 步长（按输入尺寸）
    vec3 sum = vec3(0.0);
    // 3x3 高斯权重（sigma=1.0，行和=1）
    sum += texture(inputTex, uv + vec2(-px.x, -px.y)).rgb * 0.077847;
    sum += texture(inputTex, uv + vec2( 0.0,  -px.y)).rgb * 0.123317;
    sum += texture(inputTex, uv + vec2( px.x, -px.y)).rgb * 0.077847;
    sum += texture(inputTex, uv + vec2(-px.x,  0.0)).rgb * 0.123317;
    sum += texture(inputTex, uv + vec2( 0.0,   0.0)).rgb * 0.195346;
    sum += texture(inputTex, uv + vec2( px.x,  0.0)).rgb * 0.123317;
    sum += texture(inputTex, uv + vec2(-px.x,  px.y)).rgb * 0.077847;
    sum += texture(inputTex, uv + vec2( 0.0,   px.y)).rgb * 0.123317;
    sum += texture(inputTex, uv + vec2( px.x,  px.y)).rgb * 0.077847;
    fragColor = vec4(sum, 1.0);
}
