#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <utility>
// U8,V8,widthMinusOne8,heightMinusOne8. Plane/direction are shared by a run.
struct VoxQuad { uint32_t geometry; };
static_assert(sizeof(VoxQuad)==4);
struct VoxPlaneRange {uint32_t firstQuad,quadCount,planeDirection;};
inline std::pair<uint32_t,uint32_t> DecodeVoxQuad(uint32_t packed,uint32_t planeDirection){
    const uint32_t u=packed&255u,v=(packed>>8)&255u,w=(packed>>16)&255u,h=packed>>24;
    const uint32_t d=planeDirection>>16,p=planeDirection&65535u;
    const uint32_t xyz=d<2?(u|(v<<8)|(p<<16)):(d<4?(p|(v<<8)|(u<<16)):(u|(p<<8)|(v<<16)));
    return {xyz|(w<<24),h};
}
inline void AppendVoxPlane(std::vector<VoxPlaneRange>& planes,uint32_t index,uint32_t plane,uint32_t direction){
    const uint32_t key=plane|(direction<<16);
    if(planes.empty()||planes.back().planeDirection!=key||planes.back().firstQuad+planes.back().quadCount!=index)planes.push_back({index,0,key});
    ++planes.back().quadCount;
}
inline void AppendVoxPlaneFooter(std::vector<uint32_t>& words,std::vector<VoxPlaneRange> planes,uint32_t lookup=0){
    std::sort(planes.begin(),planes.end(),[](auto a,auto b){return a.firstQuad<b.firstQuad;});
    const uint32_t base=uint32_t(words.size());
    for(auto p:planes){words.push_back(p.firstQuad);words.push_back(p.planeDirection);}
    words.push_back(base);words.push_back(uint32_t(planes.size()));words.push_back(lookup);
}
