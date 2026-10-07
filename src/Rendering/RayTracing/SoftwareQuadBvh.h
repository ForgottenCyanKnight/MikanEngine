#pragma once
#include "Rendering/RayTracing/RayTracingScene.h"
#include "Rendering/VoxRenderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Core/Log.h"
#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <numeric>

namespace mikan::rt::softquad {
// World-space binary BVH, 32-byte nodes and 16-byte quad references.
// Each reference embeds the canonical 4-byte quad plus its plane and hit identity;
// no triangle vertex/index data, and no per-candidate plane-table binary search.
// Header: node count, reference word offset, instance word offset, reference count.
inline bool Build(const std::vector<RayTracingHitInstance>& hits,
    std::vector<uint32_t>& words){
    struct Primitive {glm::vec3 lo,hi;std::array<uint32_t,4> ref;};
    struct Node {glm::vec3 lo,hi;uint32_t a,b;};
    std::vector<Primitive> primitives;
    std::vector<uint32_t> transforms(hits.size()*24u,0);
    auto bits=[](float f){return std::bit_cast<uint32_t>(f);};
    for(uint32_t index=0;index<hits.size();++index){
        const auto& hit=hits[index];if(hit.modelGeometry||!hit.geometry)return false;
        const auto* source=hit.geometry->Source();if(!source)return false;
        const float size=source->GetVoxelSize();if(!(size>0)||!std::isfinite(glm::determinant(hit.model))||std::abs(glm::determinant(hit.model))<1e-12f)return false;
        const auto inverse=glm::inverse(hit.model);std::memcpy(transforms.data()+index*24u,&inverse,sizeof(inverse));
        const auto minimum=source->GetMinBounds();
        for(uint32_t k=0;k<3;++k)transforms[index*24u+16u+k]=bits(minimum[k]);
        transforms[index*24u+19u]=bits(size);transforms[index*24u+20u]=hit.rayMask;
        uint32_t geometry=0;
        for(const auto& range:hit.geometry->Ranges()){
            for(uint32_t q=0;q<range.quadCount;++q){
                const auto decoded=source->DecodeQuad(range.firstQuad+q);
                const uint32_t g=decoded.first,a=decoded.second,d=range.direction;
                if(q>0x1fffffffu||geometry>7u)return false;
                glm::vec3 lo(float(g&255u),float((g>>8)&255u),float((g>>16)&255u)),hi=lo;
                const uint32_t axis=d<2?2:(d<4?0:1),u=d<2?0:(d<4?2:0),v=d<4?1:2;
                lo[axis]+=d==0||d==3||d==4?1.f:0.f;hi=lo;
                hi[u]+=float((g>>24)+1u);hi[v]+=float((a&255u)+1u);
                Primitive p{glm::vec3(std::numeric_limits<float>::max()),glm::vec3(-std::numeric_limits<float>::max()),
                    {source->GetQuads()[range.firstQuad+q].geometry,
                     ((g>>((d<2?2:(d<4?0:1))*8u))&255u)|(d<<16u),index,(q<<3u)|geometry}};
                for(uint32_t corner=0;corner<4;++corner){auto c=lo;c[u]=(corner&1u)?hi[u]:lo[u];c[v]=(corner&2u)?hi[v]:lo[v];
                    c=glm::vec3(hit.model*glm::vec4(minimum+c*size,1));p.lo=glm::min(p.lo,c);p.hi=glm::max(p.hi,c);}
                // Pad planar bounds for robust slab tests after world transforms.
                p.lo-=glm::vec3(1e-5f);p.hi+=glm::vec3(1e-5f);primitives.push_back(p);
            }++geometry;
        }
    }
    std::vector<uint32_t> order(primitives.size());std::iota(order.begin(),order.end(),0u);
    std::vector<Node> nodes;uint32_t maxDepth=0;
    auto area=[](glm::vec3 lo,glm::vec3 hi){auto e=glm::max(hi-lo,glm::vec3(0));return e.x*e.y+e.y*e.z+e.z*e.x;};
    // Exact centroid sweep SAH on all three axes; built only when scene inputs change.
    std::function<uint32_t(uint32_t,uint32_t,uint32_t)> build=[&](uint32_t begin,uint32_t end,uint32_t depth){
        const uint32_t node=uint32_t(nodes.size());nodes.emplace_back();maxDepth=std::max(maxDepth,depth);
        glm::vec3 lo(std::numeric_limits<float>::max()),hi(-std::numeric_limits<float>::max());
        for(uint32_t i=begin;i<end;++i){lo=glm::min(lo,primitives[order[i]].lo);hi=glm::max(hi,primitives[order[i]].hi);}
        nodes[node]={lo,hi,begin,0x80000000u|(end-begin)};
        if(end-begin<=4u||depth>=48u)return node;
        float best=std::numeric_limits<float>::max();uint32_t split=(begin+end)/2u;int bestAxis=0;
        std::vector<float> prefix(end-begin);
        for(int axis=0;axis<3;++axis){
            auto compare=[&](uint32_t a,uint32_t b){return primitives[a].lo[axis]+primitives[a].hi[axis]<primitives[b].lo[axis]+primitives[b].hi[axis];};
            std::sort(order.begin()+begin,order.begin()+end,compare);
            glm::vec3 l(std::numeric_limits<float>::max()),h(-std::numeric_limits<float>::max());
            for(uint32_t i=begin;i<end;++i){l=glm::min(l,primitives[order[i]].lo);h=glm::max(h,primitives[order[i]].hi);prefix[i-begin]=area(l,h)*float(i-begin+1u);}
            l=glm::vec3(std::numeric_limits<float>::max());h=-l;
            for(uint32_t i=end-1;i>begin;--i){l=glm::min(l,primitives[order[i]].lo);h=glm::max(h,primitives[order[i]].hi);
                const float cost=prefix[i-begin-1u]+area(l,h)*float(end-i);
                if(cost<best){best=cost;split=i;bestAxis=axis;}}
        }
        std::sort(order.begin()+begin,order.begin()+end,[&](uint32_t a,uint32_t b){return primitives[a].lo[bestAxis]+primitives[a].hi[bestAxis]<primitives[b].lo[bestAxis]+primitives[b].hi[bestAxis];});
        const uint32_t left=build(begin,split,depth+1),right=build(split,end,depth+1);
        nodes[node].a=left;nodes[node].b=right;return node;
    };
    if(!order.empty())build(0,uint32_t(order.size()),0);
    words={uint32_t(nodes.size()),4u+uint32_t(nodes.size())*8u,4u+uint32_t(nodes.size())*8u+uint32_t(order.size())*4u,uint32_t(order.size())};
    for(const auto& n:nodes){for(uint32_t k=0;k<3;++k)words.push_back(bits(n.lo[k]));words.push_back(n.a);for(uint32_t k=0;k<3;++k)words.push_back(bits(n.hi[k]));words.push_back(n.b);}
    for(auto p:order)words.insert(words.end(),primitives[p].ref.begin(),primitives[p].ref.end());
    words.insert(words.end(),transforms.begin(),transforms.end());
    LOGI("[SoftwareQuadBVH] quads=%u nodes=%u depth=%u buffer_bytes=%llu; direct rectangle intersection",uint32_t(order.size()),uint32_t(nodes.size()),maxDepth,static_cast<unsigned long long>(words.size()*4u));
    return true;
}
}
