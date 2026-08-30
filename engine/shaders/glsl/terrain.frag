#version 450

layout(set = 0, binding = 0) uniform TerrainUniformData {
    mat4 projView;
    mat4 prevProjView;
    mat4 model;
    mat4 prevModel;
    mat4 normalMatrix;
    vec4 heightParams;
    vec4 materialParams; // worldSizeZ, useControlMap, blendSharpness, reserved
    vec4 cameraPosition;
    vec4 taaJitter;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeightmap;
layout(set = 0, binding = 2) uniform sampler2D uLayer0;
layout(set = 0, binding = 3) uniform sampler2D uLayer1;
layout(set = 0, binding = 4) uniform sampler2D uLayer2;
layout(set = 0, binding = 5) uniform sampler2D uLayer3;
layout(set = 0, binding = 6) uniform sampler2D uControlMap;

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inMaterialUv;
layout(location = 3) in vec2 inHeightUv;
layout(location = 4) in vec2 inTerrainFactors;
layout(location = 5) in vec2 inMotionVector;

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

float Hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float ValueNoise(vec2 p) {
    vec2 cell = floor(p);
    vec2 local = fract(p);
    local = local * local * (3.0 - 2.0 * local);
    float a = Hash21(cell);
    float b = Hash21(cell + vec2(1.0, 0.0));
    float c = Hash21(cell + vec2(0.0, 1.0));
    float d = Hash21(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}

// 两个不共周期的缩放和旋转采样相互混合。相比单纯提高 tiling，这会隐藏
// 大片地形上非常醒目的规则重复，同时只增加一次纹理读取。
//
// uv 必须来自世界空间位置，且显式传入同一坐标的屏幕导数。地形每个
// chunk 都是独立的 draw/primitive；如果直接对 chunk 内插值的 UV 使用
// texture()，隐式 mip footprint 会在图元边界重新计算，重复纹理的周期边界
// 就会被放大成固定的块状 albedo 接缝。
vec3 SampleAntiTiled(sampler2D tex, vec2 uv, vec2 uvDx, vec2 uvDy,
                     vec2 macroCoord, float seed) {
    const mat2 rotation = mat2(0.8, -0.6, 0.6, 0.8);
    vec2 secondaryUv = rotation * (uv * 1.73) + vec2(11.7, 29.3) * seed;
    vec2 secondaryDx = rotation * (uvDx * 1.73);
    vec2 secondaryDy = rotation * (uvDy * 1.73);
    float macroNoise = ValueNoise(macroCoord + vec2(7.1, 13.9) * seed);
    float secondaryWeight = mix(0.28, 0.52, macroNoise);
    vec3 color = mix(textureGrad(tex, uv, uvDx, uvDy).rgb,
                     textureGrad(tex, secondaryUv, secondaryDx, secondaryDy).rgb,
                     secondaryWeight);
    // macroNoise 只用于选择两种纹理相位的混合权重；不再把它乘到颜色上。
    // 否则世界空间每个 noise cell 会形成与纹理内容无关的明暗块，
    // 这会被误认为是光照或 chunk 接缝。
    return color;
}

// 岩石使用世界空间三平面映射：陡坡不再把 XZ 平面纹理沿高度方向拉长。
vec3 SampleRockTriplanar(sampler2D tex, vec3 worldPosition, vec3 normal,
                         float textureFrequency, vec2 macroCoord) {
    vec3 axisWeight = pow(abs(normal), vec3(6.0));
    axisWeight /= max(axisWeight.x + axisWeight.y + axisWeight.z, 0.0001);

    vec3 worldDx = dFdx(worldPosition);
    vec3 worldDy = dFdy(worldPosition);
    vec3 xProjection = textureGrad(tex,
                                   worldPosition.zy * textureFrequency + vec2(5.3, 17.1),
                                   worldDx.zy * textureFrequency,
                                   worldDy.zy * textureFrequency).rgb;
    vec3 yProjection = textureGrad(tex,
                                   worldPosition.xz * textureFrequency + vec2(23.7, 3.9),
                                   worldDx.xz * textureFrequency,
                                   worldDy.xz * textureFrequency).rgb;
    vec3 zProjection = textureGrad(tex,
                                   worldPosition.xy * textureFrequency + vec2(41.2, 9.4),
                                   worldDx.xy * textureFrequency,
                                   worldDy.xy * textureFrequency).rgb;
    vec3 color = xProjection * axisWeight.x +
                 yProjection * axisWeight.y +
                 zProjection * axisWeight.z;
    return color;
}

float SampleTerrainHeight(vec2 uv) {
    ivec2 dimensions = max(textureSize(uHeightmap, 0), ivec2(1));
    vec2 sampleMax = vec2(max(dimensions - ivec2(1), ivec2(0)));
    vec2 texelUv = (clamp(uv, vec2(0.0), vec2(1.0)) * sampleMax + vec2(0.5)) /
                   vec2(dimensions);
    return textureLod(uHeightmap, texelUv, 0.0).r;
}

// 法线和坡度从同一张高度图逐片元计算，而不是插值各 LOD Patch 的顶点法线。
// 因此 Chunk 两侧即使顶点密度不同，也会得到相同的材质权重和 G-buffer 法线。
vec3 ComputeContinuousTerrainNormal(vec2 heightUv) {
    ivec2 dimensions = max(textureSize(uHeightmap, 0), ivec2(1));
    vec2 texelUv = 1.0 / vec2(max(dimensions - ivec2(1), ivec2(1)));
    float hL = SampleTerrainHeight(heightUv - vec2(texelUv.x, 0.0));
    float hR = SampleTerrainHeight(heightUv + vec2(texelUv.x, 0.0));
    float hD = SampleTerrainHeight(heightUv - vec2(0.0, texelUv.y));
    float hU = SampleTerrainHeight(heightUv + vec2(0.0, texelUv.y));

    float texelWorldX = max(abs(ubo.heightParams.w) /
        max(float(dimensions.x - 1), 1.0), 0.0001);
    float texelWorldZ = max(abs(ubo.materialParams.x) /
        max(float(dimensions.y - 1), 1.0), 0.0001);
    float dHdX = (hR - hL) * ubo.heightParams.x / (2.0 * texelWorldX);
    float dHdZ = (hU - hD) * ubo.heightParams.x / (2.0 * texelWorldZ);
    vec3 localNormal = normalize(vec3(-dHdX, 1.0, -dHdZ));
    return normalize(mat3(ubo.normalMatrix) * localNormal);
}

void main() {
    vec3 normal = ComputeContinuousTerrainNormal(inHeightUv);
    float slope = clamp(1.0 - normal.y, 0.0, 1.0);
    float sharpness = max(0.01, ubo.materialParams.z);

    vec4 weights;
    if (ubo.materialParams.y > 0.5) {
        // control map RGBA = layer0..layer3 权重；幂函数让过渡边界可控。
        weights = pow(max(texture(uControlMap, inHeightUv), vec4(0.0)), vec4(sharpness));
    } else {
        // 自动模式以坡度为主，不依赖不同高度图是否占满 0..1：
        // 缓坡=草地，中坡=泥土，陡坡=岩石。
        float grassToDirt = smoothstep(0.08, 0.22, slope);
        float dirtToRock = smoothstep(0.20, 0.48, slope);
        float grass = 1.0 - grassToDirt;
        float dirt = grassToDirt * (1.0 - dirtToRock);
        float rock = dirtToRock;
        weights = pow(max(vec4(grass, rock, dirt, 0.0), vec4(0.0)), vec4(sharpness));
    }
    weights /= max(dot(weights, vec4(1.0)), 0.0001);

    vec2 macroCoord = inWorldPosition.xz * 0.01;
    vec2 terrainWorldSize = max(vec2(abs(ubo.heightParams.w),
                                     abs(ubo.materialParams.x)),
                                vec2(0.0001));
    // 直接从世界位置构造材质 UV，而不是使用每个 chunk 内插值的
    // inMaterialUv。偏移保持旧的地形原点相位（局部 -size/2 对应 0）。
    vec2 materialUv = inWorldPosition.xz * (ubo.heightParams.z / terrainWorldSize) +
                      vec2(0.5 * ubo.heightParams.z);
    vec2 materialUvDx = dFdx(materialUv);
    vec2 materialUvDy = dFdy(materialUv);
    float textureFrequency = ubo.heightParams.z /
        max(max(abs(ubo.heightParams.w), abs(ubo.materialParams.x)), 1.0);
    vec3 grassAlbedo = SampleAntiTiled(uLayer0, materialUv, materialUvDx, materialUvDy,
                                       macroCoord, 1.0);
    vec3 rockAlbedo = SampleRockTriplanar(uLayer1, inWorldPosition, normal,
                                          textureFrequency, macroCoord);
    vec3 dirtAlbedo = SampleAntiTiled(uLayer2, materialUv, materialUvDx, materialUvDy,
                                      macroCoord, 2.0);
    vec3 albedo = grassAlbedo * weights.x +
                  rockAlbedo * weights.y +
                  dirtAlbedo * weights.z;
    if (weights.w > 0.0001) {
        albedo += SampleAntiTiled(uLayer3, materialUv, materialUvDx, materialUvDy,
                                  macroCoord, 3.0) * weights.w;
    }

    float roughness = mix(0.68, 0.96, clamp(0.45 * slope + 0.35 * weights.y + 0.2 * weights.w, 0.0, 1.0));

    outColor = vec4(albedo, 1.0);
    outNormal = vec4(OctahedronEncode(normal), 0.0, 0.0);
    outMaterial = vec4(0.0, roughness, 1.0, 0.0);
    outMotionVector = inMotionVector;
}
