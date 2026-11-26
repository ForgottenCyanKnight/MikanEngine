#ifndef MIKAN_WATER_WAVE_GLSL
#define MIKAN_WATER_WAVE_GLSL

// Sum the slopes of three world-space waves. Warp the sample position to
// break up repetition while keeping the surface and caustics in sync.
vec3 WaterWaveNormal(vec2 worldXZ, float timeSeconds) {
    vec2 p = worldXZ;
    p += 0.9 * vec2(sin(p.y * 0.23 + timeSeconds * 0.31),
                    sin(p.x * 0.19 - timeSeconds * 0.24));

    vec2 grad = vec2(0.0);
    const float TAU = 6.28318530718;
    vec2 d1 = normalize(vec2(0.80, 0.60));
    float k1 = TAU / 7.0;
    grad += 0.055 * k1 * d1 * cos(dot(d1, p) * k1 + timeSeconds * 1.1);
    vec2 d2 = normalize(vec2(-0.60, 0.80));
    float k2 = TAU / 3.6;
    grad += 0.032 * k2 * d2 * cos(dot(d2, p) * k2 + timeSeconds * 1.7);
    vec2 d3 = normalize(vec2(0.30, -0.95));
    float k3 = TAU / 1.8;
    grad += 0.016 * k3 * d3 * cos(dot(d3, p) * k3 + timeSeconds * 2.6);
    return normalize(vec3(-grad.x, 1.0, -grad.y));
}

#endif
