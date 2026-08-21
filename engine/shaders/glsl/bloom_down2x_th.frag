#version 450
// 2026-08-13：soft-knee 阈值提取版降采样（ds1 专用，输入 composite）
// Unity PPv2 式软膝映射：过渡带内二次曲线平滑，无硬截断；极高亮度 contribution → 1 不衰减
// 每个 tap 先提取再平均 = 先提取后降采样（后续 ds2-7 用普通核，不再重复提取）
// 参数（可调，改后重编）：BLOOM_THRESHOLD=1.0（线性 HDR），BLOOM_KNEE=0.5（过渡带 = 阈值×50%）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

const float BLOOM_THRESHOLD = 1.0;
const float BLOOM_KNEE = 0.5;

// Kawase dual 核（与 bloom_down2x_dual 一致）：中心×4 + 四对角(±1,±1)×1，÷8
vec3 Downsample(vec2 uv, vec2 px) {
    vec3 sum = texture(inputTex, uv).rgb * 4.0;
    sum += texture(inputTex, uv + vec2(-px.x,  px.y)).rgb;
    sum += texture(inputTex, uv + vec2( px.x,  px.y)).rgb;
    sum += texture(inputTex, uv + vec2(-px.x, -px.y)).rgb;
    sum += texture(inputTex, uv + vec2( px.x, -px.y)).rgb;
    return sum * 0.125;
}

// soft-knee 映射：返回提取系数（0..1 平滑过渡，无硬截断）
float SoftKneeContribution(float brightness) {
    float softKnee = BLOOM_KNEE * BLOOM_THRESHOLD;
    float soft = brightness - BLOOM_THRESHOLD + softKnee;
    soft = clamp(soft, 0.0, 2.0 * softKnee);
    soft = soft * soft / (4.0 * softKnee + 1e-4);
    return max(soft, brightness - BLOOM_THRESHOLD) / max(brightness, 1e-4);
}

void main() {
    vec2 uv = fragTexCoord;
    vec2 px = 1.0 / vec2(textureSize(inputTex, 0));
    // 中心 4 个 tap 提取
    vec3 sum = texture(inputTex, uv).rgb;
    sum *= SoftKneeContribution(dot(sum, vec3(0.2126, 0.7152, 0.0722)));
    vec3 c = texture(inputTex, uv + vec2(-px.x,  px.y)).rgb;
    sum += c * SoftKneeContribution(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    c = texture(inputTex, uv + vec2( px.x,  px.y)).rgb;
    sum += c * SoftKneeContribution(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    c = texture(inputTex, uv + vec2(-px.x, -px.y)).rgb;
    sum += c * SoftKneeContribution(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    c = texture(inputTex, uv + vec2( px.x, -px.y)).rgb;
    sum += c * SoftKneeContribution(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    fragColor = vec4(sum * 0.125, 1.0);
}
