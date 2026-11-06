#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
struct Instance {vec4 color;uvec4 info;vec4 material;};
layout(std430,binding=3) readonly buffer Instances {Instance instances[];};
#include "vox_rt_models.glsl"
#include "vox_rt_alpha.glsl"
layout(std140,binding=15) uniform Temporal {mat4 view;mat4 previousVP;mat4 previousView;vec4 jitter;vec4 options;} temporal;
layout(push_constant) uniform Push {mat4 clipFromWorld;vec4 eye;uvec4 draw;vec4 light;vec4 extent;} pc;
#ifdef MIKAN_VISIBILITY_STRIP
layout(std430,binding=2) readonly buffer Ranges {uvec4 ranges[];};
vec2 cornerUV(uint face,uint c){
 const vec2 a[4]=vec2[4](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,1));
 const vec2 b[4]=vec2[4](vec2(0,0),vec2(0,1),vec2(1,1),vec2(1,0));
 const vec2 n[4]=vec2[4](vec2(1,0),vec2(1,1),vec2(0,1),vec2(0,0));
 const vec2 d[4]=vec2[4](vec2(0,1),vec2(0,0),vec2(1,0),vec2(1,1));
 return face==0?a[c]:(face==2?n[c]:(face==5?d[c]:b[c]));
}
#endif
layout(location=0) flat in uvec3 identity;
layout(location=1) in vec2 bary;
layout(location=0) out uvec4 hit;
layout(location=1) out vec2 hitBary;
void main(){
 uint index=identity.x-1u;
 uvec3 id=identity;vec2 weights=bary;
#ifdef MIKAN_VISIBILITY_STRIP
 id.z=uint(gl_PrimitiveID);uint face=ranges[instances[index].info.x+id.y].z;
 vec2 a=cornerUV(face,0u),b=cornerUV(face,(id.z&1u)==0u?2u:3u),c=cornerUV(face,(id.z&1u)==0u?1u:2u);
 weights=inverse(mat2(b-a,c-a))*(bary-a);
#endif
 alphaStochasticSeed=alphaHash(uint(gl_FragCoord.y)*uint(pc.extent.x)+uint(gl_FragCoord.x))^uint(temporal.options.x);
 if(!acceptModelAlphaCanonical(index,id.z,weights))discard;
 // Match the two-sided canonical triangle ray-query path.

 hit=uvec4(id,floatBitsToUint(gl_FragCoord.z));hitBary=weights;
}
