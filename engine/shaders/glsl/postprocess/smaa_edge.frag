#version 450
// SMAA pass 1: luma edge detection — 官方 iryoku/smaa v2.8 SMAA.h SMAALumaEdgeDetectionPS + SMAAEdgeDetectionVS
// 严格官方：offset 逐字照抄（不做 y 翻转——官方 GLSL 版即此语义，全链自洽）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D colorTex;

const float SMAA_THRESHOLD = 0.1;

void main() {
    vec2 pixelSize = 1.0 / vec2(textureSize(colorTex, 0));

    // SMAAEdgeDetectionVS（官方原样）
    vec4 offset[3];
    offset[0] = fragTexCoord.xyxy + pixelSize.xyxy * vec4(-1.0, 0.0, 0.0, -1.0);
    offset[1] = fragTexCoord.xyxy + pixelSize.xyxy * vec4( 1.0, 0.0, 0.0,  1.0);
    offset[2] = fragTexCoord.xyxy + pixelSize.xyxy * vec4(-2.0, 0.0, 0.0, -2.0);

    // SMAALumaEdgeDetectionPS（官方原样）
    vec3 weights = vec3(0.2126, 0.7152, 0.0722);
    float L     = dot(texture(colorTex, fragTexCoord).rgb, weights);
    float Lleft = dot(texture(colorTex, offset[0].xy).rgb, weights);
    float Ltop  = dot(texture(colorTex, offset[0].zw).rgb, weights);

    vec4 delta;
    delta.xy = abs(L - vec2(Lleft, Ltop));
    vec2 edges = step(vec2(SMAA_THRESHOLD, SMAA_THRESHOLD), delta.xy);

    // 无边缘：显式写 0（不用 discard——后处理全屏覆盖，discard 像素不写入，内容依赖 loadOp 行为）
    if (dot(edges, vec2(1.0, 1.0)) == 0.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float Lright  = dot(texture(colorTex, offset[1].xy).rgb, weights);
    float Lbottom = dot(texture(colorTex, offset[1].zw).rgb, weights);
    delta.zw = abs(L - vec2(Lright, Lbottom));

    vec2 maxDelta = max(delta.xy, delta.zw);
    maxDelta = max(maxDelta.xx, maxDelta.yy);

    float Lleftleft = dot(texture(colorTex, offset[2].xy).rgb, weights);
    float Ltoptop   = dot(texture(colorTex, offset[2].zw).rgb, weights);
    delta.zw = abs(vec2(Lleft, Ltop) - vec2(Lleftleft, Ltoptop));

    maxDelta = max(maxDelta.xy, delta.zw);

    edges.xy *= step(0.5 * maxDelta, delta.xy);

    outColor = vec4(edges, 0.0, 0.0);
}
