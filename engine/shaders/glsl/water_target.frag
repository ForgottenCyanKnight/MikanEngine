#version 450

// 水面目标 RT 片元着色器（deferred water compositing 第一步，2026-09-19）：
// 水面不再写入 G-buffer/主深度（那会覆盖水底几何），改为渲染进独立 float RT。
//   R  = mask（1.0 = 有水；RT clear 值 0 = 无水，无水像素合成时直通场景色）
//   G  = gl_FragCoord.z（与主深度缓冲同一 NDC [0,1] 空间——同一投影矩阵下，
//        合成端可直接与 G-buffer 深度比较判覆盖，也可用 proj 参数线性化）
//   BA = 八面体编码的世界法线 xy（当前水面为平面，来自 water.vert 逐实例法线；
//        后续加波纹法线图时此处自然扩展）
// 覆盖判别不依赖 mask 的"不透明度"：waterZ < opaqueZ 即水面在几何之前。

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inMaterial;
layout(location = 5) in vec2 inMotionVector;

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

    outTarget = vec4(1.0, gl_FragCoord.z, OctahedronEncode(normal));
}
