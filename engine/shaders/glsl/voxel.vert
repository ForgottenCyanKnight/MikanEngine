#version 450

precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    mat4 prevProjView;
    vec3 cameraPosition;
    float padding;
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aFacePos;
layout(location = 2) in uint aPackedData;
layout(location = 3) in vec2 aSize;

// 实例数据
layout(location = 4) in mat4 aModel;
layout(location = 8) in mat4 aPrevModel;

layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragAlbedoColor;
layout(location = 3) out vec4 fragMaterialData;
layout(location = 4) out vec2 fragMotionVector;

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

vec3 rotatePosition(int faceIndex, vec3 pos, vec2 size) {
    vec3 scaledPos = vec3(pos.x * size.x, pos.y * size.y, pos.z);
    
    switch(faceIndex) {
        case 0: return vec3(-scaledPos.x, scaledPos.y, scaledPos.z);
        case 1: return vec3(scaledPos.x, scaledPos.y,-scaledPos.z);
        case 2: return vec3(-scaledPos.z, scaledPos.y, -scaledPos.x);
        case 3: return vec3(scaledPos.z, scaledPos.y, scaledPos.x);
        case 4: return vec3(scaledPos.x, scaledPos.z, scaledPos.y);
        case 5: return vec3(scaledPos.x, -scaledPos.z, -scaledPos.y);
        default: return scaledPos;
    }
}

void main() {
    uint faceDir = aPackedData & uint(0xFF);
    uint r = (aPackedData >> 8) & uint(0xFF);
    uint g = (aPackedData >> 16) & uint(0xFF);
    uint b = (aPackedData >> 24) & uint(0xFF);
    
    int faceIndex = int(faceDir);
    if (faceIndex < 0 || faceIndex > 5) {
        faceIndex = 0;
    }
    
    fragNormal = GetFaceNormal(faceIndex);
    fragAlbedoColor = vec4(float(r)/255.0, float(g)/255.0, float(b)/255.0, 1.0);
    fragMaterialData = vec4(0.5, 0.5, 1.0, 0.0);
    
    vec2 safeSize = aSize;
    if (safeSize.x <= 0.0) safeSize.x = 1.0;
    if (safeSize.y <= 0.0) safeSize.y = 1.0;
    
    vec3 localPos = aPos - vec3(0.5, 0.5, 0.0);
    vec3 rotatedPos = rotatePosition(faceIndex, localPos, safeSize);
    vec3 localWorldPos = rotatedPos + aFacePos;
    vec4 worldPos = aModel * vec4(localWorldPos, 1.0);
    vec4 prevWorldPos = aPrevModel * vec4(localWorldPos, 1.0);
    
    gl_Position = pc.projView * worldPos;
    fragPosition = worldPos.xyz;
    
    // 计算运动向量
    vec4 clipPos = pc.projView * worldPos;
    vec4 prevClipPos = pc.prevProjView * prevWorldPos;
    
    vec2 ndcPos = clipPos.xy / clipPos.w;
    vec2 prevNdcPos = prevClipPos.xy / prevClipPos.w;
    
    fragMotionVector = (ndcPos - prevNdcPos) * 0.5 + 0.5;
}
