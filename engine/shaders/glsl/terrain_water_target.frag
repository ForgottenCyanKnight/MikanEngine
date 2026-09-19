#version 450

// ===== 地形涂刷水 → 水面目标 RT 片元着色器（deferred water compositing）=====
// 地形水面不再写 G-buffer/主深度（会覆盖水底几何），改由
// TerrainRenderer::DrawWaterToTarget 画进共享 WaterTargetRT：
//   R  = mask（1.0 = 有水；clear 0 = 无水）
//   G  = 线性视距(m)（片元级 distance(world, cam)，不插值）——不存 NDC z：
//        RGBA16F 在 0.5..1 区间 ulp≈5e-4，远处一步≈数米会让水雾条纹化；
//        线性米制 fp16 在 200m 内 ulp<13cm，无可见量化。深度比较改在合成端
//        用视距进行（同射线两点，距离差 = 路径长）
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
// 视距不再用顶点插值（loc5 已弃用）：距离函数是凸函数，大三角形中点的
// 线性插值误差 ~extent²/2d，近岸大三角形可达 1m 级 → 合成端覆盖判定误拒。
// 改为片元级 distance(world, cam) 精确计算（worldPos 插值对平面三角形是精确的）。

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
    // --- 简易程序化波纹法线 ---
    // 三组方向/波长/速度错开的正弦波，解析导数求坡度，倾斜平面法线。
    // 只扰动法线不改几何：视距与覆盖判定不受影响；振幅总量 ~9° 坡度，
    // 给菲涅尔反射与太阳高光提供波光粼粼的变化。
    // 波长拉长（7.0/3.6/1.8m）+ 慢速域扭曲：纯正弦波在世界空间是严格周期
    // 的，池塘尺度上肉眼能看出重复条纹；给采样坐标加一个缓慢漂移的低频
    // 扰动（波长 ~27m，随时间漂移）打破周期性，重复感消失。振幅按
    // slope = amp·k 补偿拉长后的 k 缩小，总坡度量级保持不变。
    {
        float t = ubo.timeWind.x;
        vec2 p = inWorldPosition.xz;
        // 域扭曲：低频正弦对扰动采样点本身，波形不再随位置严格重复
        p += 0.9 * vec2(sin(p.y * 0.23 + t * 0.31), sin(p.x * 0.19 - t * 0.24));
        vec2 grad = vec2(0.0);
        const float TAU = 6.28318530718;
        vec2 d1 = normalize(vec2( 0.80, 0.60)); float k1 = TAU / 7.0;   // 长浪 7.0m
        grad += 0.055 * k1 * d1 * cos(dot(d1, p) * k1 + t * 1.1);
        vec2 d2 = normalize(vec2(-0.60, 0.80)); float k2 = TAU / 3.6;   // 中浪 3.6m
        grad += 0.032 * k2 * d2 * cos(dot(d2, p) * k2 + t * 1.7);
        vec2 d3 = normalize(vec2( 0.30,-0.95)); float k3 = TAU / 1.8;   // 涟漪 1.8m
        grad += 0.016 * k3 * d3 * cos(dot(d3, p) * k3 + t * 2.6);
        normal = normalize(vec3(-grad.x, 1.0, -grad.y));
    }
    outTarget = vec4(1.0, distance(inWorldPosition, ubo.cameraPosition.xyz), OctahedronEncode(normal));
}
