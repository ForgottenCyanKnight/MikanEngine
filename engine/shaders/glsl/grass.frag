#version 450

// 草叶片元着色器：写地形所在的 G-buffer（albedo / 法线 / 材质 / 运动矢量）。
//
// 法线策略（关键视觉优化）：
//   1. 叶面几何法线由屏幕空间导数取得（dFdx×dFdy），双面渲染下按视线翻正；
//   2. 根部 → 尖部按 t 从“地面法线”过渡到“叶面法线”：草根与地形光照融合，
//      不出现一圈黑根；叶尖保留真实朝向，保证草丛有体积感的明暗；
//   3. 全程加向上偏置，避免背光面叶片死黑。

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

layout(set = 0, binding = 2) uniform sampler2D uLayer0; // 复用地形草地表层纹理上色

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

void main() {
    vec3 worldPosition = inWorldPosT.xyz;
    float t = inWorldPosT.w;
    vec3 groundNormal = normalize(inNormalTint.xyz);
    float tint = inNormalTint.w;

    // 叶面法线：屏幕空间导数（每三角形平面法线）。
    vec3 faceNormal = normalize(cross(dFdx(worldPosition), dFdy(worldPosition)));
    vec3 viewDir = normalize(ubo.cameraPosition.xyz - worldPosition);
    // 双面渲染：让法线始终朝向相机一侧，背向面不再把叶片自己算成阴影面。
    faceNormal *= (dot(faceNormal, viewDir) < 0.0) ? 1.0 : -1.0;

    // 根部贴地、尖部立起：过渡曲线前段快速离开地面法线，避免根部“平掉”。
    float blendCurve = clamp(t * 1.35, 0.04, 1.0);
    vec3 normal = normalize(mix(groundNormal, faceNormal, blendCurve));
    // 向上偏置：草丛整体比几何法线更朝上，背光面不会死黑，受光面更柔和。
    normal = normalize(normal + vec3(0.0, 0.30 * (1.0 - 0.5 * t), 0.0));

    // 上色：复用地形草地表层纹理保持色调一致；根部环境光遮蔽 +
    // 每株 tint 变化打破均匀感；叶尖轻微黄化模拟顶部受光。
    vec2 terrainWorldSize = max(vec2(abs(ubo.heightParams.w), abs(ubo.materialParams.x)),
                                vec2(0.0001));
    vec2 materialUv = worldPosition.xz * (ubo.heightParams.z / terrainWorldSize) +
                      vec2(0.5 * ubo.heightParams.z);
    vec3 albedo = texture(uLayer0, materialUv).rgb;
    albedo *= mix(0.58, 1.0, smoothstep(0.0, 0.7, t));            // 根部 AO
    albedo *= 0.82 + 0.36 * tint;                                  // 每株色调抖动
    // 叶片比地面更"绿"一点：贴地纹理一致性保留，但草叶要能从地表里读出来。
    albedo *= vec3(0.86, 1.08, 0.78);
    albedo = mix(albedo, albedo + vec3(0.09, 0.12, 0.02),
                 smoothstep(0.6, 1.0, t));                         // 尖部受光黄化

    outColor = vec4(albedo, 1.0);
    outNormal = vec4(OctahedronEncode(normal), 0.0, 0.0);
    outMaterial = vec4(0.0, 0.85, 1.0, 0.0);
    outMotionVector = inMotionVector;
}
