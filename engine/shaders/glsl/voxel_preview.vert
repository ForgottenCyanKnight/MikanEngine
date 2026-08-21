#version 450

precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    mat4 prevProjView;
    vec3 cameraPosition;
    float padding;
} pc;

layout(location = 0) in uvec4 aPos;  // x, y, z, padding
layout(location = 1) in uvec4 aColor;  // r, g, b, padding
layout(location = 2) in uint aFaceDirAndMaterial;

layout(location = 3) in mat4 aModel;
layout(location = 7) in mat4 aPrevModel;
layout(location = 11) in vec4 aAlbedoColor;
layout(location = 12) in vec4 aMaterialData;
layout(location = 13) in vec3 aWorldMinBounds;
layout(location = 14) in float aVoxelSize;

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragAlbedoColor;
layout(location = 3) out vec4 fragMaterialData;

vec3 GetFaceNormal(int faceDir) {
    switch(faceDir) {
        case 0: return vec3(0.0, 0.0, 1.0);
        case 1: return vec3(0.0, 0.0, -1.0);
        case 2: return vec3(-1.0, 0.0, 0.0);
        case 3: return vec3(1.0, 0.0, 0.0);
        case 4: return vec3(0.0, 1.0, 0.0);
        case 5: return vec3(0.0, -1.0, 0.0);
        default: return vec3(0.0, 0.0, 1.0);
    }
}

void main() {
    int faceDir = int(aFaceDirAndMaterial & uint(0xFF));
    
    uint r = aColor.r;
    uint g = aColor.g;
    uint b = aColor.b;
    
    fragNormal = GetFaceNormal(faceDir);
    fragAlbedoColor = vec4(float(r)/255.0, float(g)/255.0, float(b)/255.0, 1.0);
    fragMaterialData = vec4(0.5, 0.5, 1.0, 0.0);
    
    vec3 relativePos = vec3(aPos.xyz) * aVoxelSize + aWorldMinBounds;
    vec4 worldPos = aModel * vec4(relativePos, 1.0);
    
    gl_Position = pc.projView * worldPos;
    fragPosition = worldPos.xyz;
}