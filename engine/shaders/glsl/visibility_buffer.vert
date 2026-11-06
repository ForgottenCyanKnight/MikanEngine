#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(std430,binding=1) readonly buffer Quads {uint quadWords[];};
#include "vox_quad_decode.glsl"
layout(std430,binding=2) readonly buffer Ranges {uvec4 ranges[];};
struct Instance {vec4 color;uvec4 info;vec4 material;};
layout(std430,binding=3) readonly buffer Instances {Instance instances[];};
layout(std430,binding=35) readonly buffer Transforms {mat4 transforms[];};
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer Words {uint words[];};
struct ModelGeometry {uvec4 addresses;uvec4 counts;vec4 emission;};
layout(std430,binding=8) readonly buffer Models {ModelGeometry models[];};
layout(push_constant) uniform Push {mat4 clipFromWorld;vec4 eye;uvec4 draw;vec4 light;vec4 extent;} pc;
layout(location=0) flat out uvec3 identity;
layout(location=1) out vec2 bary;
vec2 cornerUV(uint face,uint c){
 const vec2 a[4]=vec2[4](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,1));
 const vec2 b[4]=vec2[4](vec2(0,0),vec2(0,1),vec2(1,1),vec2(1,0));
 const vec2 c2[4]=vec2[4](vec2(1,0),vec2(1,1),vec2(0,1),vec2(0,0));
 const vec2 d[4]=vec2[4](vec2(0,1),vec2(0,0),vec2(1,0),vec2(1,1));
 return face==0?a[c]:(face==2?c2[c]:(face==5?d[c]:b[c]));
}
void main(){
 uint index=pc.draw.z!=0u?uint(gl_InstanceIndex)/256u:pc.draw.x,geometry=pc.draw.z!=0u?uint(gl_InstanceIndex)%256u:pc.draw.y,primitive=uint(gl_VertexIndex)/3u,lane=uint(gl_VertexIndex)%3u;
#ifdef MIKAN_VISIBILITY_STRIP
 primitive=(uint(gl_VertexIndex)/4u)*2u;
#endif
 Instance instance=instances[index];vec3 position;
 if(instance.info.z==1u){
  ModelGeometry m=models[instance.info.x];Words v=Words(m.addresses.xy),i=Words(m.addresses.zw);
  uint vertex=i.words[primitive*3u+lane]*8u;
  position=uintBitsToFloat(uvec3(v.words[vertex],v.words[vertex+1],v.words[vertex+2]));
 }else{
  uvec4 range=ranges[instance.info.x+geometry];uvec4 header=ranges[instance.info.w+1u];uint qi=range.x+primitive/2u-header.y;uvec4 pair=ranges[header.x+qi/2u];uvec2 q=(qi&1u)==0u?pair.xy:pair.zw;
  const uint corners[6]=uint[6](0,2,1,0,3,2);uint c=corners[(primitive&1u)*3u+lane];
#ifdef MIKAN_VISIBILITY_STRIP
 c=uint(gl_VertexIndex)%4u;
#endif
  vec2 uv=cornerUV(range.z,c)*vec2((q.x>>24)+1u,(q.y&255u)+1u);
  vec3 origin=vec3(q.x&255u,(q.x>>8)&255u,(q.x>>16)&255u);
  if(range.z==0u)origin.z+=1;else if(range.z==3u)origin.x+=1;else if(range.z==4u)origin.y+=1;
  vec3 grid=origin+(range.z<2u?vec3(uv,0):(range.z<4u?vec3(0,uv.y,uv.x):vec3(uv.x,0,uv.y)));
  vec4 metric=uintBitsToFloat(ranges[instance.info.w]);position=metric.xyz+grid*metric.w;
 }
 gl_Position=pc.clipFromWorld*transforms[index]*vec4(position,1);
#ifdef MIKAN_VISIBILITY_STRIP
 bary=cornerUV(ranges[instance.info.x+geometry].z,uint(gl_VertexIndex)%4u);
#else
 bary=lane==0u?vec2(0):(lane==1u?vec2(1,0):vec2(0,1));
#endif
 identity=uvec3(index+1u,geometry,primitive);
}
