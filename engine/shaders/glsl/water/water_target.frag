#version 450

// 水面目标 RT 片元着色器（deferred water compositing 第一步，2026-09-19）：
// 水面不再写入 G-buffer/主深度（那会覆盖水底几何），改为渲染进独立 float RT。
//   R  = mask（1.0 = 有水；RT clear 值 0 = 无水，无水像素合成时直通场景色）
//   G  = 线性视距(m)（片元级 distance(world, cam)，不插值）——不存 NDC z：
//        RGBA16F 在 0.5..1 区间 ulp≈5e-4，远处一步≈数米会让水雾条纹化；
//        深度比较在合成端用视距进行
//   BA = 八面体编码的世界法线 xy（当前水面为平面，来自 water.vert 逐实例法线；
//        后续加波纹法线图时此处自然扩展）
// 覆盖判别不依赖 mask 的"不透明度"：waterZ < opaqueZ 即水面在几何之前。

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inMaterial;
layout(location = 5) in vec2 inMotionVector;
// 视距不再用顶点插值（loc6 已弃用）：距离函数是凸函数，大三角形中点的
// 线性插值误差 ~extent²/2d，近岸大三角形可达 1m 级 → 合成端覆盖判定误拒。

layout(set = 0, binding = 0) uniform WaterUniformData {
    mat4 projView;
    mat4 prevProjView;
    vec4 cameraPosition;
    vec4 taaJitter;
} ubo;

layout(location = 0) out vec4 outTarget;

vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

vec2 OctahedronEncode(vec3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.0001);
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return n.xy;
}

void main() {
    vec3 normal = normalize(inWorldNormal);
    if (!gl_FrontFacing) normal = -normal;

    outTarget = vec4(1.0, distance(inWorldPosition, ubo.cameraPosition.xyz), OctahedronEncode(normal));
}
