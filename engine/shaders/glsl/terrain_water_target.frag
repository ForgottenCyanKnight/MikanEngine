#version 450

// ===== 地形涂刷水 → 水面目标 RT 片元着色器（deferred water compositing）=====
// 地形水面不再写 G-buffer/主深度（会覆盖水底几何），改由
// TerrainRenderer::DrawWaterToTarget 画进共享 WaterTargetRT：
//   R  = mask（1.0 = 有水；clear 0 = 无水）
//   G  = gl_FragCoord.z（与主深度缓冲同一 NDC 空间，合成端双深度比较判覆盖）
//   BA = 八面体编码的世界法线 xy（水面朝上 + 后续波纹在此扩展）
// 水深/颜色/粗糙度等材质细节留到 water_composite 全屏光照阶段再算——
// 那里能拿到无水场景色与阴影，做 Beer-Lambert 吸收、菲涅尔反射与高光。
// 主 pass 里"干区被地形深度剔除"的机制在这里不存在（RT 深度只含水面自身），
// 干区片元完全靠片元级水位守门剔除；岸边少量越界片元由合成端的
// waterZ < opaqueZ 深度比较兜底（地形在水面之前 → 无水）。

layout(set = 0, binding = 0) uniform TerrainUniformData {
    mat4 projView;
    mat4 prevProjView;
    mat4 model;
    mat4 prevModel;
    mat4 normalMatrix;
    vec4 heightParams;
    vec4 materialParams; // w = waterMaxDepth(m)
    vec4 cameraPosition;
    vec4 taaJitter;
    vec4 timeWind;
} ubo;

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inWaterParams;
layout(location = 3) in vec2 inMotionVector;
layout(location = 4) in vec2 inWaterUv;

layout(set = 0, binding = 7) uniform sampler2D uWaterMap;

layout(location = 0) out vec4 outTarget;

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
    // 片元级水位守门（与原 terrain_water.frag 同判据）：粗网格顶点间距大，
    // 干区片元（水位 texel ≈ 0）直接剔除，不写 mask。
    float waterHere = textureLod(uWaterMap, inWaterUv, 0.0).r;
    if (waterHere < 0.004) {
        discard;
    }

    vec3 normal = normalize(inWorldNormal);
    outTarget = vec4(1.0, gl_FragCoord.z, OctahedronEncode(normal));
}
