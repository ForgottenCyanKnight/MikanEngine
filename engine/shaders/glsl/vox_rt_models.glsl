#include "vox_rt_color.glsl"
#extension GL_EXT_nonuniform_qualifier : require
// Buffer-reference uvec2 addresses avoid requiring the optional shaderInt64 feature.
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer ModelWords {uint words[];};
struct ModelGeometry {uvec4 addresses;uvec4 counts;vec4 emission;};
layout(std430,binding=8) readonly buffer Models {ModelGeometry modelGeometries[];};
// Bindless model material tables. Runtime-sized arrays (descriptor indexing,
// Vulkan 1.2 core) sized by kModelTextureBindlessCapacity on the CPU side;
// each frame writes only the unique textures in use. Slot travels in
// instance.material.z / material.w; bit 31 of the albedo/emissive slot word
// requests explicit sRGB decoding (UNORM images). nonuniformEXT marks the
// instance-derived index as divergent within a wave.
layout(binding=9) uniform sampler2D modelAlbedo[];
layout(binding=21) uniform sampler2D modelMetallicRoughness[];
layout(binding=38) uniform sampler2D modelEmissive[];
layout(binding=58) uniform sampler2D modelNormal[];
// AS geometries may be partitioned/reordered. All shading, light keys and
// temporal identities use the unchanged canonical source primitive ID.
uint modelCanonicalPrimitive(uint modelIndex,uint asGeometry,uint primitive){
    ModelGeometry geometry=modelGeometries[modelIndex];
    if((geometry.counts.w&4u)==0u)return primitive;
    uint offset=asGeometry==0u?0u:(geometry.counts.w>>3u);
    ModelWords indices=ModelWords(geometry.addresses.zw);
    uint mapBase=geometry.counts.y+(geometry.counts.y/3u+15u)/16u;
    return indices.words[mapBase+offset+primitive];
}
vec3 sampleModelEmission(uint slot,vec2 uv){
    return textureLod(modelEmissive[nonuniformEXT(slot)],uv,0).rgb;
}

vec2 sampleModelMR(uint slot,vec2 uv){
    return textureLod(modelMetallicRoughness[nonuniformEXT(slot)],uv,0).gb;
}

vec3 sampleModelAlbedo(uint slot,vec2 uv){
    return textureLod(modelAlbedo[nonuniformEXT(slot)],uv,0).rgb;
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
// Any-hit alpha test (camera rays only, MASK submeshes): interpolate the UV,
// sample the albedo alpha and let the caller confirm/reject the candidate.
vec2 modelUV(uint geometryIndex,uint primitive,vec2 barycentric){
    ModelGeometry geometry=modelGeometries[geometryIndex];
    ModelWords vertices=ModelWords(geometry.addresses.xy),indices=ModelWords(geometry.addresses.zw);
    uint first=primitive*3u;
    vec3 p,n;vec2 uv0,uv1,uv2;
    modelVertex(vertices,indices.words[first],p,n,uv0);
    modelVertex(vertices,indices.words[first+1u],p,n,uv1);
    modelVertex(vertices,indices.words[first+2u],p,n,uv2);
    return uv0*(1.0-barycentric.x-barycentric.y)+uv1*barycentric.x+uv2*barycentric.y;
}
vec3 modelEmission(uint geometryIndex,uint primitive,vec2 barycentric,uint textureInfo){
    ModelGeometry geometry=modelGeometries[geometryIndex];
    // kModelEmissionGain (RayTracingScene.h): matches the voxel 128x emission
    // power so NEE lights and hit emission stay at the same magnitude.
    vec3 emission=geometry.emission.rgb*128.0;
    if(textureInfo==0xffffffffu)return emission;
    ModelWords vertices=ModelWords(geometry.addresses.xy),indices=ModelWords(geometry.addresses.zw);
    uint first=primitive*3u;
    vec3 p,n;vec2 uv0,uv1,uv2;
    modelVertex(vertices,indices.words[first],p,n,uv0);
    modelVertex(vertices,indices.words[first+1u],p,n,uv1);
    modelVertex(vertices,indices.words[first+2u],p,n,uv2);
    vec2 uv=uv0*(1.0-barycentric.x-barycentric.y)+uv1*barycentric.x+uv2*barycentric.y;
    vec3 texel=sampleModelEmission(textureInfo&0x7fffffffu,uv);
    if((textureInfo&0x80000000u)!=0u)texel=rtSrgbToLinear(texel);
    return emission*texel;
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
        color=sampleModelAlbedo(textureInfo&0x7fffffffu,uv);
        if((textureInfo&0x80000000u)!=0u)color=baseColorToLinear(color);
    }
    if(mrInfo!=0xffffffffu){
        // Same combined image the raster pipeline binds to roughness/metallic slots.
        vec2 mr=sampleModelMR(mrInfo,uv);
        roughness=clamp(mr.x,0.0,1.0);
        metallic=clamp(mr.y,0.0,1.0);
    }
}

// Preserve true triangle normals for visibility/bias; normal maps only change shading.
vec3 modelMappedWorldNormal(uint geometryIndex,uint primitive,vec2 barycentric,mat3 normalMatrix,vec3 worldNormal){
    ModelGeometry geometry=modelGeometries[geometryIndex];
    if(geometry.counts.z==0xffffffffu)return worldNormal;
    ModelWords vertices=ModelWords(geometry.addresses.xy),indices=ModelWords(geometry.addresses.zw);
    vec3 weights=vec3(1.0-barycentric.x-barycentric.y,barycentric);
    vec3 tangent=vec3(0),bitangent=vec3(0);vec2 uv=vec2(0);
    for(uint i=0u;i<3u;++i){
        uint vertex=indices.words[primitive*3u+i];vec3 p,n;vec2 tex;
        modelVertex(vertices,vertex,p,n,tex);
        vec4 t=unpackSnorm4x8(vertices.words[vertex*8u+5u]);
        if(dot(t.xyz,t.xyz)<1e-12||dot(n,n)<1e-12)return worldNormal;
        vec3 localT=normalize(t.xyz),localN=normalize(n);
        tangent+=normalize(normalMatrix*localT)*weights[i];
        bitangent+=normalize(normalMatrix*(cross(localN,localT)*t.w))*weights[i];
        uv+=tex*weights[i];
    }
    if(dot(tangent,tangent)<1e-12||dot(bitangent,bitangent)<1e-12)return worldNormal;
    vec3 sampleNormal=textureLod(modelNormal[nonuniformEXT(geometry.counts.z)],uv,0).xyz*2.0-1.0;
    // Match model.frag: packed normal assets use XY; imported/transcoded RG
    // maps may expose an empty B channel even in an RGBA/BC7 container.
    sampleNormal.z=sqrt(max(0.0,1.0-dot(sampleNormal.xy,sampleNormal.xy)));
    vec3 mapped=mat3(normalize(tangent),normalize(bitangent),worldNormal)*sampleNormal;
    return dot(mapped,mapped)>1e-12?normalize(mapped):worldNormal;
}
