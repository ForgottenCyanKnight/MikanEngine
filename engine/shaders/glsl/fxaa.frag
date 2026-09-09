#version 450
// 输入：binding 0 = 最终 LDR 图像（smaa_blend 输出，gamma 编码 8bit）
// 输出：FXAA 处理后图像（末 pass → 显示附件）
// 注意：输入已是 LDR（tonemap 之后）——luma/取色直接使用，不再需要 tonemap 转换
//   （此前 FXAA 内嵌 tonemap 时采样 HDR 合成画面，HDR 值影响边缘权重——分离后 FXAA 在 LDR 空间工作）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D colorTex;

float FxaaLuma(vec3 col) {
    return dot(col, vec3(0.299, 0.587, 0.114));
}

float fxaaQuality[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

// luma/取色：输入已是 LDR，直接采样
float fxaaLumaAt(vec2 uv) {
    return FxaaLuma(texture(colorTex, uv).rgb);
}
vec3 fxaaColorAt(vec2 uv) {
    return texture(colorTex, uv).rgb;
}

vec3 FXAA311(vec3 color, vec2 texCoord, vec2 view) {
    float edgeThresholdMin = 0.03125;
    float edgeThresholdMax = 0.125;
    float subpixelQuality = 0.75;
    int iterations = 12;

    float lumaCenter = fxaaLumaAt(texCoord);
    float lumaDown  = fxaaLumaAt(texCoord + vec2( 0.0, -1.0) * view);
    float lumaUp    = fxaaLumaAt(texCoord + vec2( 0.0,  1.0) * view);
    float lumaLeft  = fxaaLumaAt(texCoord + vec2(-1.0,  0.0) * view);
    float lumaRight = fxaaLumaAt(texCoord + vec2( 1.0,  0.0) * view);

    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));

    float lumaRange = lumaMax - lumaMin;

    if (lumaRange > max(edgeThresholdMin, lumaMax * edgeThresholdMax)) {
        float lumaDownLeft  = fxaaLumaAt(texCoord + vec2(-1.0, -1.0) * view);
        float lumaUpRight   = fxaaLumaAt(texCoord + vec2( 1.0,  1.0) * view);
        float lumaUpLeft    = fxaaLumaAt(texCoord + vec2(-1.0,  1.0) * view);
        float lumaDownRight = fxaaLumaAt(texCoord + vec2( 1.0, -1.0) * view);

        float lumaDownUp    = lumaDown + lumaUp;
        float lumaLeftRight = lumaLeft + lumaRight;

        float lumaLeftCorners  = lumaDownLeft  + lumaUpLeft;
        float lumaDownCorners  = lumaDownLeft  + lumaDownRight;
        float lumaRightCorners = lumaDownRight + lumaUpRight;
        float lumaUpCorners    = lumaUpRight   + lumaUpLeft;

        float edgeHorizontal = abs(-2.0 * lumaLeft   + lumaLeftCorners ) +
                               abs(-2.0 * lumaCenter + lumaDownUp      ) * 2.0 +
                               abs(-2.0 * lumaRight  + lumaRightCorners);
        float edgeVertical   = abs(-2.0 * lumaUp     + lumaUpCorners   ) +
                               abs(-2.0 * lumaCenter + lumaLeftRight   ) * 2.0 +
                               abs(-2.0 * lumaDown   + lumaDownCorners );

        bool isHorizontal = (edgeHorizontal >= edgeVertical);

        float luma1 = isHorizontal ? lumaDown : lumaLeft;
        float luma2 = isHorizontal ? lumaUp : lumaRight;
        float gradient1 = luma1 - lumaCenter;
        float gradient2 = luma2 - lumaCenter;

        bool is1Steepest = abs(gradient1) >= abs(gradient2);
        float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

        float stepLength = isHorizontal ? view.y : view.x;

        float lumaLocalAverage = 0.0;

        if (is1Steepest) {
            stepLength = - stepLength;
            lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
        } else {
            lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
        }

        vec2 currentUv = texCoord;
        if (isHorizontal) {
            currentUv.y += stepLength * 0.5;
        } else {
            currentUv.x += stepLength * 0.5;
        }

        vec2 offset = isHorizontal ? vec2(view.x, 0.0) : vec2(0.0, view.y);

        vec2 uv1 = currentUv - offset;
        vec2 uv2 = currentUv + offset;

        float lumaEnd1 = fxaaLumaAt(uv1);
        float lumaEnd2 = fxaaLumaAt(uv2);
        lumaEnd1 -= lumaLocalAverage;
        lumaEnd2 -= lumaLocalAverage;

        bool reached1 = abs(lumaEnd1) >= gradientScaled;
        bool reached2 = abs(lumaEnd2) >= gradientScaled;
        bool reachedBoth = reached1 && reached2;

        if (!reached1) {
            uv1 -= offset;
        }
        if (!reached2) {
            uv2 += offset;
        }

        if (!reachedBoth) {
            for(int i = 2; i < iterations; i++) {
                if (!reached1) {
                    lumaEnd1 = fxaaLumaAt(uv1);
                    lumaEnd1 = lumaEnd1 - lumaLocalAverage;
                }
                if (!reached2) {
                    lumaEnd2 = fxaaLumaAt(uv2);
                    lumaEnd2 = lumaEnd2 - lumaLocalAverage;
                }

                reached1 = abs(lumaEnd1) >= gradientScaled;
                reached2 = abs(lumaEnd2) >= gradientScaled;
                reachedBoth = reached1 && reached2;

                if (!reached1) {
                    uv1 -= offset * fxaaQuality[i];
                }
                if (!reached2) {
                    uv2 += offset * fxaaQuality[i];
                }

                if (reachedBoth) break;
            }
        }

        float distance1 = isHorizontal ? (texCoord.x - uv1.x) : (texCoord.y - uv1.y);
        float distance2 = isHorizontal ? (uv2.x - texCoord.x) : (uv2.y - texCoord.y);

        bool isDirection1 = distance1 < distance2;
        float distanceFinal = min(distance1, distance2);

        float edgeThickness = (distance1 + distance2);

        float pixelOffset = - distanceFinal / edgeThickness + 0.5;

        bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;

        bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;

        float finalOffset = correctVariation ? pixelOffset : 0.0;

        float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
        float subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
        float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
        float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * subpixelQuality;

        finalOffset = max(finalOffset, subPixelOffsetFinal);

        vec2 finalUv = texCoord;
        if (isHorizontal) {
            finalUv.y += finalOffset * stepLength;
        } else {
            finalUv.x += finalOffset * stepLength;
        }

        color = fxaaColorAt(finalUv);
    }

    return color;
}

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(colorTex, 0));
    vec3 color = texture(colorTex, fragTexCoord).rgb;
    color = FXAA311(color, fragTexCoord, texelSize)*0.98;
    outColor = vec4(color, 1.0);
}
