#version 450

// ===== 地形水位图水面片元着色器 =====
// 写入与地形相同的 G-buffer 布局（延迟光照自动生效）：
//   - albedo：浅水→深水渐变（岸边因水位笔刷软过渡自然变浅）；
//   - normal：朝上 + 两列不同方向的正弦波纹扰动（时间来自 UBO timeWind.x），
//     幅度刻意压小，只在延迟光照的低粗糙度高光里呈现微微流动感；
//   - roughness：浅水高（更像漫反射）→ 深水低（镜面感）；
//   - motion vector：静态网格走标准 current-prev 重投影（相机运动视差正确）。

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
    // 片元级水位守门：粗网格（32）顶点间距大，湖盆边缘的三角形线性插值
    // 会越过坡面浮出地形——干区片元（水位 texel ≈ 0）直接剔除。
    float waterHere = textureLod(uWaterMap, inWaterUv, 0.0).r;
    if (waterHere < 0.004) {
        discard;
    }

    float depth01 = clamp(inWaterParams.x, 0.0, 1.0);

    vec3 albedo = mix(vec3(0.10, 0.34, 0.40), vec3(0.016, 0.10, 0.17), depth01);

    // 平面水面：法线恒朝上，不做任何噪声/波纹扰动（按需求保持静止平面）。
    vec3 normal = inWorldNormal;

    float roughness = mix(0.32, 0.12, depth01);

    outColor = vec4(albedo, 1.0);
    outNormal = vec4(OctahedronEncode(normal), 0.0, 0.0);
    outMaterial = vec4(0.0, roughness, 1.0, 0.0);
    outMotionVector = inMotionVector;
}
