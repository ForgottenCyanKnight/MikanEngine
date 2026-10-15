#pragma once
// Exact emissive primitive lookup, independent of the light sampling proposal.
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mikan::rt::lights {
struct HitKey {uint32_t instance,geometry,primitive,emitter;};
inline bool Less(const HitKey& a,const HitKey& b){
    if(a.instance!=b.instance)return a.instance<b.instance;
    if(a.geometry!=b.geometry)return a.geometry<b.geometry;
    return a.primitive<b.primitive;
}
inline std::array<glm::vec3,4> VoxCorners(uint32_t g,uint32_t appearance,uint32_t direction,
    float voxelSize,glm::vec3 minBounds,const glm::mat4& model){
    static const glm::vec2 uv[6][4]={
        {{0,0},{1,0},{1,1},{0,1}},{{0,0},{0,1},{1,1},{1,0}},
        {{1,0},{1,1},{0,1},{0,0}},{{0,0},{0,1},{1,1},{1,0}},
        {{0,0},{0,1},{1,1},{1,0}},{{0,1},{0,0},{1,0},{1,1}}};
    glm::vec3 origin(float(g&255u),float((g>>8)&255u),float((g>>16)&255u));
    if(direction==0)origin.z+=1;else if(direction==3)origin.x+=1;else if(direction==4)origin.y+=1;
    std::array<glm::vec3,4> corners;
    for(uint32_t c=0;c<4;++c){const auto offset=uv[direction][c]*glm::vec2(float((g>>24)+1),float((appearance&255u)+1));
        const glm::vec3 grid=origin+(direction<2?glm::vec3(offset,0):
            (direction<4?glm::vec3(0,offset.y,offset.x):glm::vec3(offset.x,0,offset.y)));
        corners[c]=glm::vec3(model*glm::vec4(grid*voxelSize+minBounds,1));}
    return corners;
}
inline float Bits(uint32_t word){return std::bit_cast<float>(word);}
inline bool AppendIndex(std::vector<glm::vec4>& lights,uint32_t count,std::vector<HitKey> keys){
    if(lights.size()!=1ull+4ull*count||keys.size()!=count||count>(1u<<24))return false;
    if(!count){lights[0]=glm::vec4(0);return true;}
    for(uint32_t i=0;i<count;++i){
        if(lights[1+4*i].w<=0.0f)return false;
        for(uint32_t k=0;k<4;++k)for(uint32_t a=0;a<4;++a)
            if(!std::isfinite(lights[1+4*i+k][a]))return false;
    }
    std::vector<bool> indexed(count,false);
    for(const auto& key:keys){if(key.emitter>=count||indexed[key.emitter])return false;indexed[key.emitter]=true;}
    std::sort(keys.begin(),keys.end(),Less);
    for(size_t i=1;i<keys.size();++i)if(!Less(keys[i-1],keys[i]))return false;
    if(lights.size()+count>UINT32_MAX)return false;
    const uint32_t keyOffset=uint32_t(lights.size());
    for(const auto& k:keys)lights.emplace_back(Bits(k.instance),Bits(k.geometry),Bits(k.primitive),Bits(k.emitter));
    lights[0].y=0;lights[0].z=0;lights[0].w=Bits(keyOffset);
    return true;
}
}
