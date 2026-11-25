#version 450

// Camera-near waterline proxy. This is intentionally separate from the normal
// water surface compositor and is enabled only when the camera is on a nearby,
// locally sampled water mesh.

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    vec4 cameraPos;
    vec4 sunDir;
    vec4 lightColor;
    vec4 frameInfo; // xyz = air-facing local water normal, w = signed plane distance
} pc;

layout(binding = 0) uniform sampler2D sourceColor;
layout(binding = 1) uniform sampler2D sceneDepth;

layout(binding = 8) uniform CameraUBO {
    vec4 cameraPos;
    mat4 proj;
    mat4 view;
    mat4 prevViewProj;
    mat4 invProj;
    mat4 invView;
} cam;

const float WATERLINE_ACTIVATION_BAND = 0.25;
const float WATERLINE_PROXY_DISTANCE = 0.12;
const float WATER_FOG_DENSITY = 0.18;
const vec3 WATER_ABSORPTION = vec3(0.55, 0.13, 0.08);
const vec3 WATER_BODY_TINT = vec3(0.18, 0.50, 0.62);

vec3 ReconstructViewPosition(vec2 uv, float depth) {
    vec4 viewPosition = cam.invProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return viewPosition.xyz / viewPosition.w;
}

void main() {
    vec3 color = texture(sourceColor, fragTexCoord).rgb;
    float cameraSide = pc.frameInfo.w;
    if (abs(cameraSide) > WATERLINE_ACTIVATION_BAND) {
        outColor = vec4(color, 1.0);
        return;
    }

    vec3 airNormal = normalize(pc.frameInfo.xyz);
    vec3 rayView = normalize(ReconstructViewPosition(fragTexCoord, 0.5));
    vec3 rayWorld = normalize(mat3(cam.invView) * rayView);
    float rayPlaneDot = dot(rayWorld, airNormal);

    // The near-camera proxy plane represents the local tangent of the actual
    // water mesh. Classifying at a small positive ray distance gives a stable
    // half-submerged split without modifying ordinary water pixels elsewhere.
    float sideAtProxy = cameraSide + rayPlaneDot * WATERLINE_PROXY_DISTANCE;
    float edgeWidth = max(fwidth(sideAtProxy), 0.0015);
    float underwaterWeight = 1.0 - smoothstep(-edgeWidth, edgeWidth, sideAtProxy);
    if (underwaterWeight <= 0.001) {
        outColor = vec4(color, 1.0);
        return;
    }

    float depthSample = texture(sceneDepth, fragTexCoord).r;
    bool hasGeometry = depthSample < 0.999999;
    float sceneDistance = hasGeometry
        ? length(ReconstructViewPosition(fragTexCoord, depthSample))
        : 24.0;

    // Integrate only the segment on the submerged side of the local plane.
    // This gives the fog distance from the waterline crossing to the visible
    // scene point, rather than tinting by camera-to-water height alone.
    float waterStart = 0.0;
    float waterEnd = sceneDistance;
    if (cameraSide > 0.0 && rayPlaneDot < -1e-5) {
        waterStart = clamp(-cameraSide / rayPlaneDot, 0.0, sceneDistance);
    } else if (cameraSide < 0.0 && rayPlaneDot > 1e-5) {
        waterEnd = min(sceneDistance, max(-cameraSide / rayPlaneDot, 0.0));
    }
    float underwaterDistance = max(waterEnd - waterStart, 0.0);

    float exposure = max(pc.sunDir.w, 0.0);
    vec3 sunDirection = pc.sunDir.xyz;
    float sunLengthSquared = dot(sunDirection, sunDirection);
    sunDirection = sunLengthSquared > 1e-6
        ? sunDirection * inversesqrt(sunLengthSquared)
        : vec3(0.0, 1.0, 0.0);
    float waterSun = max(dot(sunDirection, airNormal), 0.0);
    vec3 sunLight = max(pc.lightColor.rgb, vec3(0.0));
    vec3 fogColor = max(
        WATER_BODY_TINT * (vec3(0.18) + sunLight * exposure * waterSun * 0.08) * 0.7,
        vec3(0.03, 0.08, 0.10) * exposure);
    float fogAmount = 1.0 - exp(-WATER_FOG_DENSITY * underwaterDistance);
    vec3 transmittance = exp(-WATER_ABSORPTION * underwaterDistance);
    vec3 foggedColor = color * transmittance + fogColor * fogAmount;

    outColor = vec4(mix(color, foggedColor, underwaterWeight), 1.0);
}
