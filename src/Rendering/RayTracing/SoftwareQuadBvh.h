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
#include <cstdlib>

namespace mikan::rt::softquad {
// Static world-space binary BVH. Compact format: 16 B conservative UNORM16
// nodes, 8 B quad references, 64 B world-to-grid instance records.
// MIKAN_SOFT_QUAD_COMPACT=0 retains the 32/16/96 B reference format.
// Compact references share canonical quad words and face ranges; the legacy
// layout embeds quad/plane/identity. Neither stores triangle vertices or indices.
// Header: node count, reference word offset, instance word offset, reference count.
// Compact headers also contain the root quantization origin and per-axis step.
inline bool Build(const std::vector<RayTracingHitInstance>& hits,
    std::vector<uint32_t>& words){
    const char* option=std::getenv("MIKAN_SOFT_QUAD_COMPACT");
    const bool compact=!option||std::strcmp(option,"0")!=0;
    if(compact&&hits.size()>0x1000000u)return false;
    struct Primitive {glm::vec3 lo,hi;std::array<uint32_t,4> ref;};
    struct Node {glm::vec3 lo,hi;uint32_t a,b;};
    std::vector<Primitive> primitives;
    const uint32_t transformStride=compact?16u:24u;
    std::vector<uint32_t> transforms(hits.size()*transformStride,0);
    auto bits=[](float f){return std::bit_cast<uint32_t>(f);};
    for(uint32_t index=0;index<hits.size();++index){
        const auto& hit=hits[index];if(hit.modelGeometry||!hit.geometry)return false;
        const auto* source=hit.geometry->Source();if(!source)return false;
        const float size=source->GetVoxelSize();if(!(size>0)||!std::isfinite(glm::determinant(hit.model))||std::abs(glm::determinant(hit.model))<1e-12f)return false;
        const auto inverse=glm::inverse(hit.model);const auto minimum=source->GetMinBounds();
        if(compact){
            // Affine rows already include minBounds and voxelSize. Ray t is unchanged.
            for(uint32_t row=0;row<3;++row)for(uint32_t col=0;col<4;++col)
                transforms[index*16u+row*4u+col]=bits((inverse[col][row]-(col==3u?minimum[row]:0.0f))/size);
            transforms[index*16u+12u]=hit.rayMask;
        }else{
            std::memcpy(transforms.data()+index*24u,&inverse,sizeof(inverse));
            for(uint32_t k=0;k<3;++k)transforms[index*24u+16u+k]=bits(minimum[k]);
            transforms[index*24u+19u]=bits(size);transforms[index*24u+20u]=hit.rayMask;
        }
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
    if(compact&&order.size()>0x1000000u)return false;
    const uint32_t header=compact?12u:4u,nodeStride=compact?4u:8u,refStride=compact?2u:4u;
    words={uint32_t(nodes.size()),header+uint32_t(nodes.size())*nodeStride,
        header+uint32_t(nodes.size())*nodeStride+uint32_t(order.size())*refStride,uint32_t(order.size())};
    glm::vec3 base(0),step(1);
    if(compact){
        if(!nodes.empty()){
            base=nodes[0].lo;
            for(uint32_t k=0;k<3;++k){
                step[k]=std::nextafter((nodes[0].hi[k]-base[k])/65535.0f,std::numeric_limits<float>::infinity());
                if(!(step[k]>0.0f)||!std::isfinite(step[k]))return false;
                while(base[k]+step[k]*65535.0f<nodes[0].hi[k])step[k]=std::nextafter(step[k],std::numeric_limits<float>::infinity());
            }
        }
        for(uint32_t k=0;k<3;++k)words.push_back(bits(base[k]));words.push_back(0u);
        for(uint32_t k=0;k<3;++k)words.push_back(bits(step[k]));words.push_back(0u);
    }
    for(const auto& n:nodes){
        if(compact){
            uint32_t lo[3],hi[3];
            for(uint32_t k=0;k<3;++k){
                // Outward rounding plus one cell protects float reconstruction/FMA.
                const double a=(double(n.lo[k])-base[k])/step[k],b=(double(n.hi[k])-base[k])/step[k];
                lo[k]=uint32_t(std::clamp(std::floor(a)-1.0,0.0,65535.0));
                hi[k]=uint32_t(std::clamp(std::ceil(b)+1.0,0.0,65535.0));
                if(base[k]+float(lo[k])*step[k]>n.lo[k]||base[k]+float(hi[k])*step[k]<n.hi[k])return false;
            }
            uint32_t link=n.b;
            if(n.b&0x80000000u){const uint32_t count=n.b&0x7fffffffu;if(count>127u)return false;link=0x80000000u|(count<<24u)|n.a;}
            else if(n.a!=uint32_t(&n-nodes.data())+1u)return false;
            words.insert(words.end(),{lo[0]|(lo[1]<<16u),lo[2]|(hi[0]<<16u),hi[1]|(hi[2]<<16u),link});
        }else{for(uint32_t k=0;k<3;++k)words.push_back(bits(n.lo[k]));words.push_back(n.a);for(uint32_t k=0;k<3;++k)words.push_back(bits(n.hi[k]));words.push_back(n.b);}
    }
    for(auto p:order){const auto& ref=primitives[p].ref;
        if(compact)words.insert(words.end(),{(ref[1]&255u)|(ref[2]<<8u),ref[3]});
        else words.insert(words.end(),ref.begin(),ref.end());
    }
    words.insert(words.end(),transforms.begin(),transforms.end());
    LOGI("[SoftwareQuadBVH] quads=%u nodes=%u depth=%u buffer_bytes=%llu; direct rectangle intersection",uint32_t(order.size()),uint32_t(nodes.size()),maxDepth,static_cast<unsigned long long>(words.size()*4u));
    LOGI("[SoftwareQuadBVH layout] compact=%u nodeBytes=%u refBytes=%u instanceBytes=%u",compact?1u:0u,nodeStride*4u,refStride*4u,transformStride*4u);
    return true;
}
}
