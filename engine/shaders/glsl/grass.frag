#version 450

// 草叶片元着色器：写地形所在的 G-buffer（albedo / 法线 / 材质 / 运动矢量）。
//
// 法线策略（用户拍板 2026-09-18）：全部草地像素法线 = 世界正上 (0,1,0)。
// 叶面法线（屏幕导数）+ 根部过渡的方案在密集草丛里光照不可控，且草影
// 位置错位会放大观感问题——先统一按平地受光，等草影修复后再回归。

layout(set = 0, binding = 0) uniform TerrainUniformData {
    mat4 projView;
    mat4 prevProjView;
    mat4 model;
    mat4 prevModel;
    mat4 normalMatrix;
    vec4 heightParams;   // heightScale, heightOffset, materialTiling, worldSizeX
    vec4 materialParams; // worldSizeZ, useControlMap, blendSharpness, reserved
    vec4 cameraPosition;
    vec4 taaJitter;
    vec4 timeWind;       // time(s), windStrength, grassViewDistance, heightGain
} ubo;

layout(location = 0) in vec4 inWorldPosT;  // worldPos.xyz, t
layout(location = 1) in vec4 inNormalTint; // groundNormal.xyz, tint
layout(location = 2) in vec2 inMotionVector;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec2 outMotionVector;

vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

vec2 OctahedronEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return n.xy;
}

// 上色（用户拍板 2026-09-18）：单色平涂——噪声色斑/每株 tint/根部 AO/
// 尖部黄化全部去掉。观感不均匀的来源就是这些叠加变化。
void main() {
    vec3 worldPosition = inWorldPosT.xyz;
    float t = inWorldPosT.w;

    // 法线 = 世界正上：草丛整体按平地受光（与地面一致，无叶面朝向噪声）。
    vec3 normal = vec3(0.0, 1.0, 0.0);

    vec3 albedo = vec3(0.25, 0.42, 0.11);                           // 单色草绿

    outColor = vec4(albedo, 1.0);
    outNormal = vec4(OctahedronEncode(normal), 0.0, 0.0);
    outMaterial = vec4(0.0, 0.85, 1.0, 0.0);
    outMotionVector = inMotionVector;
}
