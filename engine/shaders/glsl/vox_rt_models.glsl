#include "vox_rt_color.glsl"
// Buffer-reference uvec2 addresses avoid requiring the optional shaderInt64 feature.
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer ModelWords {uint words[];};
struct ModelGeometry {uvec4 addresses;uvec4 counts;};
layout(std430,binding=8) readonly buffer Models {ModelGeometry modelGeometries[];};
layout(binding=9) uniform sampler2D modelAlbedo[8];
// glTF metallicRoughness textures (g=roughness, b=metallic), same 8-slot
// constant-index scheme as albedo. Slot travels in instance.material.z.
layout(binding=21) uniform sampler2D modelMetallicRoughness[8];

vec2 sampleModelMR(uint slot,vec2 uv){
    switch(slot){
        case 0:return textureLod(modelMetallicRoughness[0],uv,0).gb;
        case 1:return textureLod(modelMetallicRoughness[1],uv,0).gb;
        case 2:return textureLod(modelMetallicRoughness[2],uv,0).gb;
        case 3:return textureLod(modelMetallicRoughness[3],uv,0).gb;
        case 4:return textureLod(modelMetallicRoughness[4],uv,0).gb;
        case 5:return textureLod(modelMetallicRoughness[5],uv,0).gb;
        case 6:return textureLod(modelMetallicRoughness[6],uv,0).gb;
        case 7:return textureLod(modelMetallicRoughness[7],uv,0).gb;
        default:return vec2(1.0,0.0);
    }
}

// Constant descriptor indices keep this first implementation independent of
// sampled-image nonuniform indexing features. All eight slots are valid.
vec3 sampleModelAlbedo(uint slot,vec2 uv){
    switch(slot){
        case 0:return textureLod(modelAlbedo[0],uv,0).rgb;
        case 1:return textureLod(modelAlbedo[1],uv,0).rgb;
        case 2:return textureLod(modelAlbedo[2],uv,0).rgb;
        case 3:return textureLod(modelAlbedo[3],uv,0).rgb;
        case 4:return textureLod(modelAlbedo[4],uv,0).rgb;
        case 5:return textureLod(modelAlbedo[5],uv,0).rgb;
        case 6:return textureLod(modelAlbedo[6],uv,0).rgb;
        case 7:return textureLod(modelAlbedo[7],uv,0).rgb;
        default:return vec3(1);
    }
}
vec3 baseColorToLinear(vec3 color){
    return rtSrgbToLinear(color);
}
void modelVertex(ModelWords vertices,uint index,out vec3 position,out vec3 normal,out vec2 uv){
    uint base=index*8u;
    position=uintBitsToFloat(uvec3(vertices.words[base],vertices.words[base+1],vertices.words[base+2]));
    normal=unpackSnorm4x8(vertices.words[base+3]).xyz;
    uv=unpackHalf2x16(vertices.words[base+4]);
}
void modelSurface(uint geometryIndex,uint primitive,vec2 barycentric,uint textureInfo,uint mrInfo,
    float metalFallback,float roughFallback,
    out vec3 color,out vec3 normal,out vec3 geometricNormal,out float metallic,out float roughness){
    ModelGeometry geometry=modelGeometries[geometryIndex];
    ModelWords vertices=ModelWords(geometry.addresses.xy),indices=ModelWords(geometry.addresses.zw);
    uint first=primitive*3u;
    vec3 p0,p1,p2,n0,n1,n2;vec2 uv0,uv1,uv2;
    modelVertex(vertices,indices.words[first],p0,n0,uv0);
    modelVertex(vertices,indices.words[first+1],p1,n1,uv1);
    modelVertex(vertices,indices.words[first+2],p2,n2,uv2);
    vec3 weights=vec3(1-barycentric.x-barycentric.y,barycentric);
    geometricNormal=normalize(cross(p1-p0,p2-p0));
    normal=n0*weights.x+n1*weights.y+n2*weights.z;
    normal=dot(normal,normal)>1e-8?normalize(normal):geometricNormal;
    color=vec3(1);
    vec2 uv=uv0*weights.x+uv1*weights.y+uv2*weights.z;
    metallic=metalFallback;roughness=roughFallback;
    if(textureInfo!=0xffffffffu){
        color=sampleModelAlbedo(textureInfo&255u,uv);
        if((textureInfo&256u)!=0u)color=baseColorToLinear(color);
    }
    if(mrInfo!=0xffffffffu){
        // Same combined image the raster pipeline binds to roughness/metallic slots.
        vec2 mr=sampleModelMR(mrInfo&255u,uv);
        roughness=clamp(mr.x,0.0,1.0);
        metallic=clamp(mr.y,0.0,1.0);
    }
}
