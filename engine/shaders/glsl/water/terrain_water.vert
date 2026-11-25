#version 450

// ===== 地形水位图水面网格顶点着色器 =====
// 覆盖整块地形的静态网格（无实例），顶点高度 = 地形高度 + 水位图水深：
//   - 有水 texel：水面抬升到 原地形高度（水位笔刷挖低地形 = 水深），
//     所以水面正好贴在"涂水前的地面"上；
//   - 无水 texel：下沉 4cm 藏进地形深度里，避免与地形共面 z-fighting，
//     干燥区域的三角形全部被地形深度剔除，零片元成本。
// 水位图/高度图同分辨率同原点，UV 直接复用地形网格的 0..1 坐标。
// 与 terrain.* 共用描述符布局（binding 7 水位图在 VS 采样）与 per-frame UBO。

layout(set = 0, binding = 0) uniform TerrainUniformData {
    mat4 projView;
    mat4 prevProjView;
    mat4 model;
    mat4 prevModel;
    mat4 normalMatrix;
    vec4 heightParams;   // heightScale, heightOffset, materialTiling, worldSizeX
    vec4 materialParams; // worldSizeZ, useControlMap, blendSharpness, waterMaxDepth(m)
    vec4 cameraPosition;
    vec4 taaJitter;
    vec4 timeWind;       // time(s), windStrength, grassViewDistance, heightGain
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeightmap;
layout(set = 0, binding = 7) uniform sampler2D uWaterMap;

layout(location = 0) in vec2 inPatchPosition;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outWaterParams; // x = depth01
layout(location = 3) out vec2 outMotionVector;
layout(location = 4) out vec2 outWaterUv;     // 片元级水位采样用（粗网格防伪影）
layout(location = 5) out float outViewDist;   // 线性视距(m)：fp16 存 NDC z 远处一步≈数米，条纹化

float SampleHeight(vec2 uv) {
    // 与 terrain.vert 同一套"采样点域"修正：把 0..1 映射到 texel 中心。
    ivec2 dimensions = max(textureSize(uHeightmap, 0), ivec2(1));
    vec2 sampleMax = vec2(max(dimensions - ivec2(1), ivec2(0)));
    vec2 texelUv = (clamp(uv, vec2(0.0), vec2(1.0)) * sampleMax + vec2(0.5)) /
                   vec2(dimensions);
    return texture(uHeightmap, texelUv).r;
}

float SampleWater(vec2 uv) {
    ivec2 dimensions = max(textureSize(uWaterMap, 0), ivec2(1));
    vec2 sampleMax = vec2(max(dimensions - ivec2(1), ivec2(0)));
    vec2 texelUv = (clamp(uv, vec2(0.0), vec2(1.0)) * sampleMax + vec2(0.5)) /
                   vec2(dimensions);
    return texture(uWaterMap, texelUv).r;
}

void main() {
    vec2 uv = clamp(inPatchPosition, vec2(0.0), vec2(1.0));

    float normalizedHeight = SampleHeight(uv);
    float terrainLocalY = normalizedHeight * ubo.heightParams.x + ubo.heightParams.y;

    float waterRaw = SampleWater(uv);
    float depthMeters = waterRaw * max(ubo.materialParams.w, 0.0001);
    // 岸线过渡带（2cm 水深内）把下沉量平滑插值到抬升量，避免硬边。
    float surfaceOffset = mix(-0.04, depthMeters, smoothstep(0.0, 0.02, waterRaw));

    vec2 terrainWorldSize = max(vec2(abs(ubo.heightParams.w),
                                     abs(ubo.materialParams.x)),
                                vec2(0.0001));
    vec2 localXZ = -0.5 * terrainWorldSize + uv * terrainWorldSize;
    vec3 localPosition = vec3(localXZ.x, terrainLocalY + surfaceOffset, localXZ.y);

    vec4 worldPosition = ubo.model * vec4(localPosition, 1.0);
    vec4 clipPosition = ubo.projView * worldPosition;
    clipPosition.xy += ubo.taaJitter.xy * clipPosition.w;
    gl_Position = clipPosition;

    vec4 previousWorldPosition = ubo.prevModel * vec4(localPosition, 1.0);
    vec4 previousClipPosition = ubo.prevProjView * previousWorldPosition;
    vec2 currentNdc = clipPosition.xy / max(abs(clipPosition.w), 0.000001);
    vec2 previousNdc = previousClipPosition.xy / max(abs(previousClipPosition.w), 0.000001);

    outWorldPosition = worldPosition.xyz;
    outViewDist = distance(worldPosition.xyz, ubo.cameraPosition.xyz);
    outWorldNormal = normalize(mat3(ubo.normalMatrix) * vec3(0.0, 1.0, 0.0));
    outWaterParams = vec2(clamp(waterRaw, 0.0, 1.0), 0.0);
    outMotionVector = (currentNdc - previousNdc) * 0.5;
    outWaterUv = uv;
}
