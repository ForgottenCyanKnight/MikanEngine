#version 450
// SMAA pass 3: neighborhood blending — 官方 iryoku/smaa v2.8 SMAA.h SMAANeighborhoodBlendingPS + SMAANeighborhoodBlendingVS
// 严格官方：逐字照抄（不做 y 翻转——官方 GLSL_4 分支即 bilinear 偏移采样）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D colorTex;
layout(binding = 1) uniform sampler2D blendTex;

void main() {
    vec2 pixelSize = 1.0 / vec2(textureSize(colorTex, 0));

    // SMAANeighborhoodBlendingVS（官方原样）
    vec4 offset[2];
    offset[0] = fragTexCoord.xyxy + pixelSize.xyxy * vec4(-1.0, 0.0, 0.0, -1.0);
    offset[1] = fragTexCoord.xyxy + pixelSize.xyxy * vec4( 1.0, 0.0, 0.0,  1.0);

    // SMAANeighborhoodBlendingPS（官方原样）
    vec4 a;
    a.xz = texture(blendTex, fragTexCoord).xz;
    a.y = texture(blendTex, offset[1].zw).g;
    a.w = texture(blendTex, offset[1].xy).a;

    if (fragTexCoord.x<0.5||dot(a, vec4(1.0, 1.0, 1.0, 1.0)) < 1e-5) {
        outColor = texture(colorTex, fragTexCoord);
        return;
    }

    vec2 offset2;
    offset2.x = a.a > a.b ? a.a : -a.b;   // left vs. right
    offset2.y = a.g > a.r ? a.g : -a.r;   // top vs. bottom

    if (abs(offset2.x) > abs(offset2.y))
        offset2.y = 0.0;
    else
        offset2.x = 0.0;

    // GLSL_4 分支：bilinear 偏移采样混合
    vec2 tc = fragTexCoord + offset2 * pixelSize;
    outColor = texture(colorTex, tc);
    if(fragTexCoord.x<0.5) outColor = texture(colorTex, fragTexCoord);
}
