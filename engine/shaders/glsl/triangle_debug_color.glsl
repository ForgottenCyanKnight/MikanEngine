// Stable avalanche hash: consecutive primitive IDs must not form color bands.
uint triangleDebugHash(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}
vec3 triangleDebugColor(uint primitive) {
    uint h = triangleDebugHash(primitive);
    return vec3(0.15) + vec3(h & 255u, (h >> 8u) & 255u, (h >> 16u) & 255u) * (0.8 / 255.0);
}
