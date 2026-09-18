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

// 上色：硬编码草地色 + 噪声混合（不采样纹理，移动端省一次纹理带宽）：
//   - 低频块噪声（2m 网格 hash）模拟草地色斑/干湿差异；
//   - 细噪声打破块边界，避免格子感；
//   - 根部 AO + 每株 tint 抖动保留；叶尖轻微黄化模拟顶部受光。
float Hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec3 worldPosition = inWorldPosT.xyz;
    float t = inWorldPosT.w;
    float tint = inNormalTint.w;

    // 法线 = 世界正上：草丛整体按平地受光（与地面一致，无叶面朝向噪声）。
    vec3 normal = vec3(0.0, 1.0, 0.0);

    // 硬编码基色：暗绿（湿/阴）→ 亮黄绿（干/晒），块噪声插值 + 细噪声扰动。
    vec2 patchCell = floor(worldPosition.xz * 0.5);
    float patchNoise = Hash12(patchCell);
    float fineNoise = Hash12(floor(worldPosition.xz * 4.0));
    vec3 albedo = mix(vec3(0.16, 0.30, 0.08), vec3(0.33, 0.48, 0.14), patchNoise);
    albedo = mix(albedo, vec3(0.24, 0.42, 0.10), fineNoise * 0.35);
    // 根部 AO：绿色调压暗（不是中性灰暗化）——暗下去的同时不丢饱和度，
    // 贴根段不会从草绿褪成灰绿。
    albedo *= mix(vec3(0.40, 0.54, 0.34), vec3(1.0), smoothstep(0.0, 0.7, t));
    albedo *= 0.82 + 0.36 * tint;                                  // 每株色调抖动
    albedo = mix(albedo, albedo + vec3(0.09, 0.12, 0.02),
                 smoothstep(0.6, 1.0, t));                         // 尖部受光黄化

    outColor = vec4(albedo, 1.0);
    outNormal = vec4(OctahedronEncode(normal), 0.0, 0.0);
    outMaterial = vec4(0.0, 0.85, 1.0, 0.0);
    outMotionVector = inMotionVector;
}
