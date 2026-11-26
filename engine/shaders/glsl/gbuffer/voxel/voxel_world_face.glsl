#ifndef MIKAN_VOXEL_WORLD_FACE_GLSL
#define MIKAN_VOXEL_WORLD_FACE_GLSL

vec2 unpackSizeFromPacked(uint packedData) {
    float sizeX = 1.0 + float((packedData >> 24) & 0xF);
    float sizeY = 1.0 + float((packedData >> 28) & 0xF);
    return vec2(sizeX, sizeY);
}

vec3 rotatePosition(int faceIndex, vec3 pos) {
    switch (faceIndex) {
        case 0: return pos;                         // Z+
        case 1: return vec3(-pos.x, pos.y, -pos.z);  // Z-
        case 2: return vec3(-pos.z, pos.y, pos.x);   // X-
        case 3: return vec3(pos.z, pos.y, -pos.x);   // X+
        case 4: return vec3(pos.x, -pos.z, -pos.y);  // Y+
        case 5: return vec3(pos.x, pos.z, pos.y);    // Y-
        default: return pos;
    }
}

const vec3 faceNormals[6] = vec3[6](
    vec3(0.0, 0.0, 1.0),
    vec3(0.0, 0.0, -1.0),
    vec3(-1.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, -1.0, 0.0)
);

#endif
