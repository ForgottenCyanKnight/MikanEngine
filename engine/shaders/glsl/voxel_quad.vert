#version 450
#extension GL_GOOGLE_include_directive : require

precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    mat4 prevProjView;
    vec3 cameraPosition;
    float padding;
} pc;

layout(std430,binding=1) readonly buffer Quads { uint quadWords[]; };
#include "vox_quad_decode.glsl"

layout(location = 3) in uint sourceInstance;
layout(binding = 0) uniform samplerBuffer sourceData;
layout(location = 0) out vec3 fragPosition;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragAlbedoColor;
layout(location = 3) out vec4 fragMaterialData;
layout(location = 4) out vec2 fragMotionVector;
layout(location=5) flat out uint surfaceQuad;
layout(location=6) out vec2 surfaceUV;
layout(location=7) flat out vec3 surfaceTint;

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
    int base = int(sourceInstance / 6u) * 12;
    mat4 aModel = mat4(texelFetch(sourceData, base), texelFetch(sourceData, base + 1),
        texelFetch(sourceData, base + 2), texelFetch(sourceData, base + 3));
    mat4 aPrevModel = mat4(texelFetch(sourceData, base + 4), texelFetch(sourceData, base + 5),
        texelFetch(sourceData, base + 6), texelFetch(sourceData, base + 7));
    vec4 aAlbedoColor = texelFetch(sourceData, base + 8);
    vec4 minSize = texelFetch(sourceData, base + 10);
    vec3 aWorldMinBounds = minSize.xyz;
    float aVoxelSize = minSize.w;
    uint q=uint(gl_VertexIndex)/4u;
    uvec2 decoded=voxDecodeQuad(q);uint geometry=decoded.x,appearance=decoded.y;
    int faceDir=int(sourceInstance % 6u);
    vec3 origin=vec3(geometry&255u,(geometry>>8)&255u,(geometry>>16)&255u);
    if(faceDir==0)origin.z+=1.0;
    else if(faceDir==3)origin.x+=1.0;
    else if(faceDir==4)origin.y+=1.0;
    vec2 size=vec2((geometry>>24)+1u,(appearance&255u)+1u);
    int corner=int(uint(gl_VertexIndex)%4u);
    const vec2 positiveZ[4]=vec2[4](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,1));
    const vec2 negativeZ[4]=vec2[4](vec2(0,0),vec2(0,1),vec2(1,1),vec2(1,0));
    const vec2 negativeX[4]=vec2[4](vec2(1,0),vec2(1,1),vec2(0,1),vec2(0,0));
    const vec2 positiveX[4]=vec2[4](vec2(0,0),vec2(0,1),vec2(1,1),vec2(1,0));
    const vec2 positiveY[4]=vec2[4](vec2(0,0),vec2(0,1),vec2(1,1),vec2(1,0));
    const vec2 negativeY[4]=vec2[4](vec2(0,1),vec2(0,0),vec2(1,0),vec2(1,1));
    vec2 uv=faceDir==0?positiveZ[corner]:(faceDir==1?negativeZ[corner]:(faceDir==2?negativeX[corner]:(faceDir==3?positiveX[corner]:(faceDir==4?positiveY[corner]:negativeY[corner]))));
    surfaceQuad=q;surfaceUV=uv;surfaceTint=aAlbedoColor.rgb;
    vec2 offset=uv*size;
    vec3 grid=origin+(faceDir<2?vec3(offset,0):(faceDir<4?vec3(0,offset.y,offset.x):vec3(offset.x,0,offset.y)));
    uvec3 rgb=uvec3((appearance>>8)&255u,(appearance>>16)&255u,(appearance>>24)&255u);
    // 使用面方向计算法线
    fragNormal = normalize(transpose(inverse(mat3(aModel))) * GetFaceNormal(faceDir));
    
    // 使用顶点颜色（取前 3 个分量）
    fragAlbedoColor = aAlbedoColor;
    
    // 使用硬编码材质或从材质索引获取
    // 这里暂时使用默认材质
    fragMaterialData = vec4(0.0, 1.0, 1.0, 0.0);
    
    vec3 relativePos = grid * aVoxelSize + aWorldMinBounds;
    vec4 worldPos = aModel * vec4(relativePos, 1.0);
    
    gl_Position = pc.projView * worldPos;
    vec4 previousClip = pc.prevProjView * aPrevModel * vec4(relativePos, 1.0);
    fragMotionVector = (gl_Position.w > 0.0001 && previousClip.w > 0.0001)
        ? (gl_Position.xy / gl_Position.w - previousClip.xy / previousClip.w) * 0.5
        : vec2(0.0);
    fragPosition = worldPos.xyz;
}
