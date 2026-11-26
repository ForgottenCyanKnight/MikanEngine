#version 450
#extension GL_GOOGLE_include_directive : require

// World-anchored caustic bake. This pass maps a camera-centered, texel-snapped
// XZ region to a fixed-resolution animated irradiance texture; it does not
// read depth, the water mask, or any screen-space scene image.

precision highp float;
#include "../water/water_wave.glsl"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    vec4 cameraPos;
    vec4 sunDir;
    vec4 lightColor; // .w = same animation time as terrain_water_target.frag
    vec4 frameInfo;  // .yz = this bake target's pixel dimensions
} pc;

// No sampler inputs are needed. PostProcessChain reserves bindings 0..7, so
// the shared camera UBO remains at binding 8 for this pass.
layout(binding = 8) uniform CameraUBO {
    vec4 cameraPos;
    mat4 proj;
    mat4 view;
    mat4 prevViewProj;
    mat4 invProj;
    mat4 invView;
} cam;

const float CAUSTIC_MAP_WORLD_WIDTH = 384.0;
const float WATER_IOR = 1.0 / 1.333;

vec2 TraceSourceXZ(vec2 receiverXZ, float waterDepth, vec3 toSun) {
    vec2 sourceXZ = receiverXZ;
    for (int iteration = 0; iteration < 4; ++iteration) {
        vec3 normal = WaterWaveNormal(sourceXZ, pc.lightColor.w);
        vec3 refractedLight = refract(-toSun, normal, WATER_IOR);
        float travel = waterDepth / max(-refractedLight.y, 0.05);
        vec2 hitXZ = receiverXZ - refractedLight.xz * travel;
        sourceXZ = mix(sourceXZ, hitXZ, 0.8);
    }
    return sourceXZ;
}

float Cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

float BakeFocus(vec2 sourceXZ, float waterDepth, float receiverArea) {
    vec2 sourceDx = dFdx(sourceXZ);
    vec2 sourceDy = dFdy(sourceXZ);
    float sourceArea = abs(Cross2(sourceDx, sourceDy));
    float concentration = sourceArea / max(receiverArea, 1e-8);
    return clamp(concentration - 1.0, 0.0, 3.0) * exp(-0.22 * waterDepth);
}

void main() {
    vec2 mapSize = max(pc.frameInfo.yz, vec2(1.0));
    vec2 mapSpan = vec2(CAUSTIC_MAP_WORLD_WIDTH,
        CAUSTIC_MAP_WORLD_WIDTH * mapSize.y / mapSize.x);
    // Keep the map origin fixed in world space while the camera stays inside
    // this tile. It shifts by a whole map span only when crossing a tile edge,
    // avoiding per-texel re-baking as the camera moves.
    vec2 mapCenter = floor(pc.cameraPos.xz / mapSpan + 0.5) * mapSpan;
    vec2 receiverXZ = mapCenter + (fragTexCoord - 0.5) * mapSpan;

    vec3 toSun = normalize(pc.sunDir.xyz);
    if (toSun.y <= 0.01) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 receiverDx = dFdx(receiverXZ);
    vec2 receiverDy = dFdy(receiverXZ);
    float receiverArea = abs(Cross2(receiverDx, receiverDy));

    // Store four depth slices. A two-slice 0.5m/4m blend overlays patterns
    // whose refracted source coordinates have shifted by meters, obscuring the
    // wave-normal correspondence at the receiver depth.
    const vec4 DEPTHS = vec4(0.5, 1.5, 2.5, 4.0);
    float focus0 = BakeFocus(TraceSourceXZ(receiverXZ, DEPTHS.x, toSun), DEPTHS.x, receiverArea);
    float focus1 = BakeFocus(TraceSourceXZ(receiverXZ, DEPTHS.y, toSun), DEPTHS.y, receiverArea);
    float focus2 = BakeFocus(TraceSourceXZ(receiverXZ, DEPTHS.z, toSun), DEPTHS.z, receiverArea);
    float focus3 = BakeFocus(TraceSourceXZ(receiverXZ, DEPTHS.w, toSun), DEPTHS.w, receiverArea);

    // RGBA store focus at 0.5m/1.5m/2.5m/4.0m respectively.
    outColor = vec4(focus0, focus1, focus2, focus3);
}
