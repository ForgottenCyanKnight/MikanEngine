#version 450
// SMAA pass 2: blending weight calculation — 官方 iryoku/smaa v2.8 SMAA.h SMAABlendingWeightCalculationPS + SMAABlendingWeightCalculationVS
// 严格官方：搜索/面积/角点/主函数逐字照抄（不做 y 翻转——官方 GLSL 版即此语义，全链自洽）
// 输入：binding 0 = edgesTex（RG8, Linear）、binding 1 = areaTex（R8G8, Linear）、binding 2 = searchTex（R8, Point/Nearest）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D edgesTex;
layout(binding = 1) uniform sampler2D areaTex;
layout(binding = 2) uniform sampler2D searchTex;

// 官方常量（HIGH 预设）
const int SMAA_MAX_SEARCH_STEPS = 32;
const int SMAA_MAX_SEARCH_STEPS_DIAG = 0;   // 跳过对角（LOW 行为：省 areaTex 右半，无需 SMAAAreaDiag）
const int SMAA_CORNER_ROUNDING = 25;
const float SMAA_AREATEX_MAX_DISTANCE = 16.0;
const float SMAA_AREATEX_PIXEL_SIZE_X = 1.0 / 160.0;
const float SMAA_AREATEX_PIXEL_SIZE_Y = 1.0 / 560.0;
const float SMAA_AREATEX_SUBTEX_SIZE = 1.0 / 7.0;

// ---- 官方 SMAASearchLength（v2.8 参数化；searchTex 必须 Point 采样）----
float SMAASearchLength(vec2 e, float bias, float scale) {
    e.r = bias + e.r * scale;
    return 255.0 * texture(searchTex, e).r;
}

// ---- 官方水平搜索（PSEUDO_GATHER4）----
float SMAASearchXLeft(vec2 texcoord, float end) {
    vec2 pixelSize = 1.0 / vec2(textureSize(edgesTex, 0));
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x > end &&
           e.g > 0.8281 &&
           e.r == 0.0) {
        e = texture(edgesTex, texcoord).rg;
        texcoord -= vec2(2.0, 0.0) * pixelSize;
    }
    texcoord.x += 0.25 * pixelSize.x;
    texcoord.x += pixelSize.x;
    texcoord.x += 2.0 * pixelSize.x;
    texcoord.x -= pixelSize.x * SMAASearchLength(e, 0.0, 0.5);
    return texcoord.x;
}

float SMAASearchXRight(vec2 texcoord, float end) {
    vec2 pixelSize = 1.0 / vec2(textureSize(edgesTex, 0));
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x < end &&
           e.g > 0.8281 &&
           e.r == 0.0) {
        e = texture(edgesTex, texcoord).rg;
        texcoord += vec2(2.0, 0.0) * pixelSize;
    }
    texcoord.x -= 0.25 * pixelSize.x;
    texcoord.x -= pixelSize.x;
    texcoord.x -= 2.0 * pixelSize.x;
    texcoord.x += pixelSize.x * SMAASearchLength(e, 0.5, 0.5);
    return texcoord.x;
}

float SMAASearchYUp(vec2 texcoord, float end) {
    vec2 pixelSize = 1.0 / vec2(textureSize(edgesTex, 0));
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y > end &&
           e.r > 0.8281 &&
           e.g == 0.0) {
        e = texture(edgesTex, texcoord).rg;
        texcoord -= vec2(0.0, 2.0) * pixelSize;
    }
    texcoord.y += 0.25 * pixelSize.y;
    texcoord.y += pixelSize.y;
    texcoord.y += 2.0 * pixelSize.y;
    texcoord.y -= pixelSize.y * SMAASearchLength(e.gr, 0.0, 0.5);
    return texcoord.y;
}

float SMAASearchYDown(vec2 texcoord, float end) {
    vec2 pixelSize = 1.0 / vec2(textureSize(edgesTex, 0));
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y < end &&
           e.r > 0.8281 &&
           e.g == 0.0) {
        e = texture(edgesTex, texcoord).rg;
        texcoord += vec2(0.0, 2.0) * pixelSize;
    }
    texcoord.y -= 0.25 * pixelSize.y;
    texcoord.y -= pixelSize.y;
    texcoord.y -= 2.0 * pixelSize.y;
    texcoord.y += pixelSize.y * SMAASearchLength(e.gr, 0.5, 0.5);
    return texcoord.y;
}

// ---- 官方 SMAAArea（areaTex R8G8、二次压缩）----
vec2 SMAAArea(vec2 dist, float e1, float e2, float offset) {
    vec2 texcoord = vec2(SMAA_AREATEX_MAX_DISTANCE) * round(4.0 * vec2(e1, e2)) + dist;
    texcoord = vec2(SMAA_AREATEX_PIXEL_SIZE_X, SMAA_AREATEX_PIXEL_SIZE_Y) * texcoord +
               vec2(0.5 * SMAA_AREATEX_PIXEL_SIZE_X, 0.5 * SMAA_AREATEX_PIXEL_SIZE_Y);
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return texture(areaTex, texcoord).rg;
}

// ---- 官方角点检测（SMAA_CORNER_ROUNDING=25）----
void SMAADetectHorizontalCornerPattern(inout vec2 weights, vec2 texcoord, vec2 d) {
    vec2 pixelSize = 1.0 / vec2(textureSize(edgesTex, 0));
    vec4 coords = fma(vec4(d.x, 0.0, d.y, 0.0), pixelSize.xyxy, texcoord.xyxy);
    vec2 e;
    e.r = textureLodOffset(edgesTex, coords.xy, 0.0, ivec2(0,  1)).r;
    bool left = abs(d.x) < abs(d.y);
    e.g = textureLodOffset(edgesTex, coords.xy, 0.0, ivec2(0, -2)).r;
    if (left) weights *= clamp(float(SMAA_CORNER_ROUNDING) / 100.0 + 1.0 - e, 0.0, 1.0);

    e.r = textureLodOffset(edgesTex, coords.zw, 0.0, ivec2(1,  1)).r;
    e.g = textureLodOffset(edgesTex, coords.zw, 0.0, ivec2(1, -2)).r;
    if (!left) weights *= clamp(float(SMAA_CORNER_ROUNDING) / 100.0 + 1.0 - e, 0.0, 1.0);
}

void SMAADetectVerticalCornerPattern(inout vec2 weights, vec2 texcoord, vec2 d) {
    vec2 pixelSize = 1.0 / vec2(textureSize(edgesTex, 0));
    vec4 coords = fma(vec4(0.0, d.x, 0.0, d.y), pixelSize.xyxy, texcoord.xyxy);
    vec2 e;
    e.r = textureLodOffset(edgesTex, coords.xy, 0.0, ivec2( 1, 0)).g;
    bool left = abs(d.x) < abs(d.y);
    e.g = textureLodOffset(edgesTex, coords.xy, 0.0, ivec2(-2, 0)).g;
    if (left) weights *= clamp(float(SMAA_CORNER_ROUNDING) / 100.0 + 1.0 - e, 0.0, 1.0);

    e.r = textureLodOffset(edgesTex, coords.zw, 0.0, ivec2( 1, 1)).g;
    e.g = textureLodOffset(edgesTex, coords.zw, 0.0, ivec2(-2, 1)).g;
    if (!left) weights *= clamp(float(SMAA_CORNER_ROUNDING) / 100.0 + 1.0 - e, 0.0, 1.0);
}

// ---- 官方主函数（VS offset 计算内联；pixcoord 由 fragTexCoord 推导）----
void main() {
    vec2 pixelSize = 1.0 / vec2(textureSize(edgesTex, 0));
    vec4 weights = vec4(0.0, 0.0, 0.0, 0.0);

    // SMAABlendingWeightCalculationVS（官方原样）
    vec4 offset[3];
    offset[0] = fragTexCoord.xyxy + pixelSize.xyxy * vec4(-0.25, -0.125,  1.25, -0.125);
    offset[1] = fragTexCoord.xyxy + pixelSize.xyxy * vec4(-0.125, -0.25, -0.125,  1.25);
    offset[2] = vec4(offset[0].xz, offset[1].yw) +
                vec4(-2.0, 2.0, -2.0, 2.0) * pixelSize.xxyy * float(SMAA_MAX_SEARCH_STEPS);

    vec2 e = texture(edgesTex, fragTexCoord).rg;

    if (e.g > 0.0) {   // 水平边缘（north）
        vec2 d;

        vec2 coords;
        coords.x = SMAASearchXLeft(offset[0].xy, offset[2].x);
        coords.y = offset[1].y;
        d.x = coords.x;

        float e1 = texture(edgesTex, coords).r;

        coords.x = SMAASearchXRight(offset[0].zw, offset[2].y);
        d.y = coords.x;

        d = d / pixelSize.x - fragTexCoord.x / pixelSize.x;

        vec2 sqrt_d = sqrt(abs(d));

        float e2 = textureLodOffset(edgesTex, coords, 0.0, ivec2(1, 0)).r;

        weights.rg = SMAAArea(sqrt_d, e1, e2, 0.0);

        SMAADetectHorizontalCornerPattern(weights.rg, fragTexCoord, d);
    }

    if (e.r > 0.0) {   // 垂直边缘（west）
        vec2 d;

        vec2 coords;
        coords.y = SMAASearchYUp(offset[1].xy, offset[2].z);
        coords.x = offset[0].x;
        d.x = coords.y;

        float e1 = texture(edgesTex, coords).g;

        coords.y = SMAASearchYDown(offset[1].zw, offset[2].w);
        d.y = coords.y;

        d = d / pixelSize.y - fragTexCoord.y / pixelSize.y;

        vec2 sqrt_d = sqrt(abs(d));

        float e2 = textureLodOffset(edgesTex, coords, 0.0, ivec2(0, 1)).g;

        weights.ba = SMAAArea(sqrt_d, e1, e2, 0.0);

        SMAADetectVerticalCornerPattern(weights.ba, fragTexCoord, d);
    }

    outColor = weights;
}
