#version 450

// 草叶顶点着色器：零顶点缓冲，叶片几何由二次贝塞尔曲线程序化生成。
// 每实例 32 字节（局部位置 + 形状参数），条带拓扑 10 顶点 = 4 段，
// 顶点索引 v: row = v/2 ∈ [0,4]，side = v%2 ? +1 : -1，row=4 时宽度归零收尖。
//
// 高度不进实例：Y 每帧从 16-bit 高度图采样，雕刻地形后草自动贴地；
// 法线在片元里做"地面法线 → 叶面法线"过渡（见 grass.frag）。

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

// CSM 级联矩阵走 push constant（与 terrain_csm_depth.vert 同一约定）：
// 一条命令缓冲里会录制多个级联 + 主 pass，它们共享同一份 per-frame
// host-visible UBO，录制期的多次写入只有最后一次生效——级联矩阵写
// UBO 会让所有级联都拿到主相机的 projView，草就投不出影子。
// 阴影 pass 传 useCsm.x=1 + 级联矩阵；主 pass 传 useCsm.x=0（用 UBO）。
layout(push_constant) uniform GrassPush {
    mat4 csmProjView;
    vec4 csmParams;      // x = useCsm
} push;

layout(set = 0, binding = 1) uniform sampler2D uHeightmap;

layout(location = 0) in vec4 inPosParams;   // localX, localZ, yaw, height
layout(location = 1) in vec4 inShapeParams; // width, bend, phase, tint

layout(location = 0) out vec4 outWorldPosT;     // worldPos.xyz, t (0=根 1=尖)
layout(location = 1) out vec4 outNormalTint;    // worldGroundNormal.xyz, tint
layout(location = 2) out vec2 outMotionVector;

float SampleHeight(vec2 uv) {
    // 与 terrain.vert 相同的“采样点域”映射，保证草的贴地高度和地形表面一致。
    ivec2 dimensions = max(textureSize(uHeightmap, 0), ivec2(1));
    vec2 sampleMax = vec2(max(dimensions - ivec2(1), ivec2(0)));
    vec2 texelUv = (clamp(uv, vec2(0.0), vec2(1.0)) * sampleMax + vec2(0.5)) /
                   vec2(dimensions);
    return textureLod(uHeightmap, texelUv, 0.0).r;
}

void main() {
    const int SEGMENTS = 4;
    int row = gl_VertexIndex / 2;               // 0..4
    float side = (gl_VertexIndex % 2 == 0) ? -1.0 : 1.0;
    float t = float(row) / float(SEGMENTS);

    float localX = inPosParams.x;
    float localZ = inPosParams.y;
    float yaw = inPosParams.z;
    float h = inPosParams.w * max(ubo.timeWind.w, 0.01f);
    float width = inShapeParams.x;
    float bend = inShapeParams.y;
    float phase = inShapeParams.z;

    vec2 worldSize = max(vec2(abs(ubo.heightParams.w), abs(ubo.materialParams.x)),
                         vec2(0.0001));
    vec2 heightUv = clamp(vec2(localX, localZ) / worldSize + vec2(0.5), vec2(0.0), vec2(1.0));
    float baseY = SampleHeight(heightUv) * ubo.heightParams.x + ubo.heightParams.y;

    // 地面法线（局部空间，差分同 terrain.vert），片元里与叶面法线混合。
    ivec2 dims = max(textureSize(uHeightmap, 0), ivec2(1));
    vec2 texel = 1.0 / vec2(max(dims - ivec2(1), ivec2(1)));
    float texelWorldX = max(abs(ubo.heightParams.w) / max(float(dims.x - 1), 1.0), 0.0001);
    float texelWorldZ = max(abs(ubo.materialParams.x) / max(float(dims.y - 1), 1.0), 0.0001);
    float hL = SampleHeight(heightUv - vec2(texel.x, 0.0));
    float hR = SampleHeight(heightUv + vec2(texel.x, 0.0));
    float hD = SampleHeight(heightUv - vec2(0.0, texel.y));
    float hU = SampleHeight(heightUv + vec2(0.0, texel.y));
    float dHdX = (hR - hL) * ubo.heightParams.x / (2.0 * texelWorldX);
    float dHdZ = (hU - hD) * ubo.heightParams.x / (2.0 * texelWorldZ);
    vec3 groundNormal = normalize(mat3(ubo.normalMatrix) *
                                  normalize(vec3(-dHdX, 1.0, -dHdZ)));

    // 贝塞尔控制点：P0 在根部，P1 控制中段弯曲，P2 是叶尖。
    vec3 sideDir = vec3(cos(yaw), 0.0, sin(yaw));
    vec3 fwdDir = vec3(-sin(yaw), 0.0, cos(yaw));
    vec3 p1 = vec3(0.0, 0.55 * h, 0.0) + fwdDir * (bend * 0.5 * h);
    vec3 p2 = vec3(0.0, h, 0.0) + fwdDir * (bend * h);

    // 风摆：时间相位 + 空间相位（叶片间错开），幅度随 t^2 增长（根部不动尖部摆）。
    float swayPhase = ubo.timeWind.x * 2.0 + phase + localX * 0.15 + localZ * 0.11;
    float sway = sin(swayPhase) * 0.12 + 0.35 * sin(swayPhase * 1.83 + 1.7);
    sway *= ubo.timeWind.y * h * t * t;
    vec2 windDir = vec2(0.86, 0.5);
    p1.xz += windDir * sway * 0.5;
    p2.xz += windDir * sway;

    // B(t) = 2(1-t)t·P1 + t²·P2（P0 = 原点）。
    vec3 blade = 2.0 * (1.0 - t) * t * p1 + t * t * p2;
    // 宽度线性收尖，row=4 时两侧顶点重合于叶尖。
    float halfWidth = 0.5 * width * (1.0 - t);

    // ==== 距离 LOD + 逐株溶解（确定性，逐株稳定，主 pass 与阴影 pass 一致）====
    // 视距 45% 起按距离二次方随机抽稀（溶解阈值随距离升到 1.0，最远处全剔），
    // 幸存远叶按比例增宽补偿覆盖面积——远处草丛密度感不塌，顶点/光栅化
    // 负载大幅下降。抽稀用叶片世界坐标的 hash，逐帧稳定，不会闪烁；
    // 阴影 pass 用同一份着色器，投影的草与渲染的草严格一致。
    vec4 rootWorld = ubo.model * vec4(localX, baseY, localZ, 1.0);
    float viewDist = max(ubo.timeWind.z, 1.0);
    float distanceXZ = distance(ubo.cameraPosition.xz, rootWorld.xz);
    float lodFade = clamp((distanceXZ - 0.45 * viewDist) / (0.55 * viewDist), 0.0, 1.0);
    float bladeHash = fract(sin(dot(vec2(localX, localZ), vec2(12.9898, 78.233))) * 43758.5453);
    bool culled = (distanceXZ > viewDist) ||
                  (bladeHash < lodFade * lodFade);
    halfWidth *= 1.0 + lodFade * 2.0;

    if (culled) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outWorldPosT = vec4(0.0);
        outNormalTint = vec4(0.0, 1.0, 0.0, 0.0);
        outMotionVector = vec2(0.0);
        return;
    }

    const bool shadowPass = push.csmParams.x > 0.5;
    const mat4 viewProj = shadowPass ? push.csmProjView : ubo.projView;
    vec3 localPosition = vec3(localX, baseY, localZ) + blade + sideDir * (side * halfWidth);
    vec4 worldPosition = ubo.model * vec4(localPosition, 1.0);
    vec4 clipPosition = viewProj * worldPosition;
    if (!shadowPass) {
        clipPosition.xy += ubo.taaJitter.xy * clipPosition.w;
    }
    gl_Position = clipPosition;

    // 运动矢量用“不带风摆的基准位置”重投影：风摆是高频小幅位移，
    // 让 TAA 逐帧追踪它只会得到糊成一团的草。
    vec3 baseLocal = vec3(localX, baseY, localZ);
    vec4 previousWorldPosition = ubo.prevModel * vec4(baseLocal, 1.0);
    vec4 previousClipPosition = ubo.prevProjView * previousWorldPosition;
    vec2 currentNdc = clipPosition.xy / max(abs(clipPosition.w), 0.000001);
    vec2 previousNdc = previousClipPosition.xy / max(abs(previousClipPosition.w), 0.000001);

    outWorldPosT = vec4(worldPosition.xyz, t);
    outNormalTint = vec4(groundNormal, inShapeParams.w);
    outMotionVector = (currentNdc - previousNdc) * 0.5;
}
