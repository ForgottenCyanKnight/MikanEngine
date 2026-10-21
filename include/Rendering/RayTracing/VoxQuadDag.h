#pragma once
#include "Rendering/VoxQuad.h"
#include <array>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <limits>

namespace mikan::rt {
struct DagQuad {uint32_t x,y,z,width,height,face,source;};
inline bool DagQuadLess(const DagQuad& a,const DagQuad& b){
    auto key=[](const DagQuad& q){return std::array<uint32_t,6>{q.face,q.face<2?q.z:(q.face<4?q.x:q.y),q.face<2?q.x:(q.face<4?q.z:q.x),q.face<4?q.y:q.z,q.width,q.height};};
    return key(a)<key(b);
}
struct VoxDagSurface {
    std::array<uint32_t,3> origin{};
    uint32_t node=0;
    std::vector<VoxQuad> quads;
    std::vector<VoxPlaneRange> planes;
    std::vector<uint32_t> sources,directions;
};
struct VoxQuadDag {
    // Leaf: {0,face,width,height}. Branch: {1,edge,count,(child,dx,dy,dz)...}.
    // Material and placement identity are intentionally outside interned nodes.
    std::vector<std::vector<uint32_t>> nodes;
    std::vector<VoxDagSurface> surfaces;
    uint32_t root=0,uniqueBlocks=0;
    size_t uniqueQuads=0;
};
inline VoxQuadDag CompileVoxQuadDag(std::span<const DagQuad> input,uint32_t selectedEdge){
    if(selectedEdge!=3&&selectedEdge!=9&&selectedEdge!=27&&selectedEdge!=81)throw std::runtime_error("DAG edge must be 3,9,27,81");
    VoxQuadDag out;if(input.empty())return out;
    std::map<std::vector<uint32_t>,uint32_t> intern;
    auto node=[&](std::vector<uint32_t> words){
        auto [it,added]=intern.emplace(words,uint32_t(out.nodes.size()));
        if(added)out.nodes.push_back(std::move(words));return it->second;
    };
    struct Cell {uint32_t node;std::vector<uint32_t> items;};
    using Coord=std::array<uint32_t,3>;
    std::map<Coord,Cell> cells;
    // Keep multiple differently oriented quads at a coordinate as separate leaves.
    std::map<Coord,std::vector<uint32_t>> anchors;
    for(uint32_t i=0;i<input.size();++i){const auto& q=input[i];
        if(q.face>5||!q.width||!q.height||q.width>256||q.height>256||q.x>255||q.y>255||q.z>255)throw std::runtime_error("Invalid DAG quad");
        anchors[{q.x,q.y,q.z}].push_back(i);
    }
    for(auto& [pos,items]:anchors){std::vector<uint32_t> children;
        for(auto i:items){const auto& q=input[i];children.push_back(node({0,q.face,q.width,q.height}));}
        std::sort(children.begin(),children.end());std::vector<uint32_t> words{1,1,uint32_t(children.size())};
        for(auto id:children)words.insert(words.end(),{id,0,0,0});
        cells.emplace(pos,Cell{node(std::move(words)),std::move(items)});
    }
    std::set<uint32_t> selectedNodes;
    for(uint32_t edge=3;edge<=729;edge*=3){
        std::map<Coord,std::vector<std::pair<Coord,Cell>>> groups;
        for(auto& [p,c]:cells){Coord base{p[0]/edge*edge,p[1]/edge*edge,p[2]/edge*edge};groups[base].emplace_back(p,std::move(c));}
        cells.clear();
        for(auto& [base,children]:groups){
            std::vector<std::array<uint32_t,4>> refs;std::vector<uint32_t> items;
            for(auto& [p,c]:children){refs.push_back({c.node,p[0]-base[0],p[1]-base[1],p[2]-base[2]});items.insert(items.end(),c.items.begin(),c.items.end());}
            std::sort(refs.begin(),refs.end());std::vector<uint32_t> words{1,edge,uint32_t(refs.size())};
            for(auto ref:refs)words.insert(words.end(),ref.begin(),ref.end());
            const auto id=node(std::move(words));
            if(edge==selectedEdge){
                std::sort(items.begin(),items.end(),[&](uint32_t a,uint32_t b){const auto& x=input[a];const auto& y=input[b];return DagQuadLess(x,y);});
                VoxDagSurface surface;surface.origin=base;surface.node=id;
                for(auto index:items){auto q=input[index];q.x-=base[0];q.y-=base[1];q.z-=base[2];
                    const uint32_t u=q.face<2?q.x:(q.face<4?q.z:q.x),v=q.face<4?q.y:q.z,plane=q.face<2?q.z:(q.face<4?q.x:q.y);
                    AppendVoxPlane(surface.planes,uint32_t(surface.quads.size()),plane,q.face);
                    surface.quads.push_back({u|(v<<8)|((q.width-1)<<16)|((q.height-1)<<24)});
                    surface.sources.push_back(q.source);surface.directions.push_back(q.face);
                }
                if(selectedNodes.insert(id).second)out.uniqueQuads+=items.size();
                out.surfaces.push_back(std::move(surface));
            }
            cells.emplace(base,Cell{id,std::move(items)});
        }
        if(cells.size()==1&&edge>=selectedEdge){out.root=cells.begin()->second.node;break;}
    }
    out.uniqueBlocks=uint32_t(selectedNodes.size());return out;
}
// Choose a DAG cut with an explicit instance budget. Rare/uneconomic nodes
// become one residual triangle BLAS, rather than one BLAS per spatial cell.
inline void SelectVoxDagCut(VoxQuadDag& dag,std::span<const DagQuad> input,uint32_t maxInstances=4096,uint32_t maxUnique=96){
    struct Candidate{uint32_t node,count,quads;int64_t benefit;};
    std::map<uint32_t,Candidate> counts;
    for(const auto& surface:dag.surfaces){auto [it,added]=counts.emplace(surface.node,Candidate{surface.node,0,uint32_t(surface.quads.size()),0});++it->second.count;}
    std::vector<Candidate> candidates;
    for(auto [node,c]:counts){
        // Approximation only; actual compact AS+TLAS+shader buffers are measured.
        c.benefit=int64_t(c.count-1)*c.quads*56-int64_t(c.count)*512-1024;
        if(c.count>=2&&c.quads>=8&&c.benefit>0)candidates.push_back(c);
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.benefit*int64_t(b.count)>b.benefit*int64_t(a.count);});
    std::set<uint32_t> selected;uint32_t instances=1;
    for(const auto& c:candidates)if(instances+c.count<=maxInstances&&selected.size()+1<maxUnique){selected.insert(c.node);instances+=c.count;}
    std::vector<VoxDagSurface> kept;std::vector<uint32_t> residual;
    std::set<uint32_t> represented;
    for(const auto& surface:dag.surfaces)represented.insert(surface.sources.begin(),surface.sources.end());
    for(const auto& q:input)if(!represented.contains(q.source))residual.push_back(q.source);
    std::map<uint32_t,const DagQuad*> originals;for(const auto& q:input)originals.emplace(q.source,&q);
    dag.uniqueQuads=0;for(auto id:selected)dag.uniqueQuads+=counts.at(id).quads;
    for(auto& surface:dag.surfaces){if(selected.contains(surface.node))kept.push_back(std::move(surface));else residual.insert(residual.end(),surface.sources.begin(),surface.sources.end());}
    if(!residual.empty()){
        std::sort(residual.begin(),residual.end(),[&](uint32_t a,uint32_t b){const auto& x=*originals.at(a);const auto& y=*originals.at(b);return DagQuadLess(x,y);});
        VoxDagSurface tail;tail.node=UINT32_MAX;
        for(auto id:residual){const auto& q=*originals.at(id);const uint32_t u=q.face<2?q.x:(q.face<4?q.z:q.x),v=q.face<4?q.y:q.z,plane=q.face<2?q.z:(q.face<4?q.x:q.y);
            AppendVoxPlane(tail.planes,uint32_t(tail.quads.size()),plane,q.face);
            tail.quads.push_back({u|(v<<8)|((q.width-1)<<16)|((q.height-1)<<24)});tail.sources.push_back(id);tail.directions.push_back(q.face);
        }
        dag.uniqueQuads+=tail.quads.size();kept.push_back(std::move(tail));
    }
    dag.surfaces=std::move(kept);dag.uniqueBlocks=uint32_t(selected.size())+uint32_t(!residual.empty());
}
// Reorder descriptors while retaining exact palette and R8 samples. Byte offsets
// are relocated; no resampling or new palette quantization takes place.
inline std::vector<uint32_t> SliceVoxDagAttributes(std::span<const uint32_t> original,
    std::span<const uint32_t> ids,std::span<const VoxQuad> quads){
    if(original.empty())return {};
    if(original.size()<4||original[0]<4||original[0]>original[1]||original[1]>original[2]||original[2]>original.size()||ids.size()!=quads.size())throw std::runtime_error("Invalid DAG attributes");
    std::vector<uint32_t> result{4,4+original[1]-original[0],4+original[1]-original[0]+uint32_t(ids.size()),0};
    result.insert(result.end(),original.begin()+original[0],original.begin()+original[1]);
    const uint32_t descriptors=uint32_t(result.size());result.resize(result.size()+ids.size());
    std::vector<uint8_t> bytes;std::map<std::pair<uint32_t,uint32_t>,uint32_t> copied;
    for(size_t i=0;i<ids.size();++i){
        if(uint64_t(original[1])+ids[i]>=original[2])throw std::runtime_error("DAG descriptor index");
        uint32_t value=original[original[1]+ids[i]];
        if(value&0x80000000u){const uint32_t offset=value&0x7fffffffu,n=((quads[i].geometry>>16)&255u)+1u,m=(quads[i].geometry>>24)+1u;
            auto [it,fresh]=copied.emplace(std::make_pair(offset,n*m),uint32_t(bytes.size()));
            if(fresh)for(uint32_t k=0;k<n*m;++k){const uint64_t at=uint64_t(original[2])+(uint64_t(offset)+k)/4;
                if(at>=original.size())throw std::runtime_error("DAG R8 bounds");bytes.push_back(uint8_t(original[at]>>(((offset+k)&3u)*8u)));}
            value=0x80000000u|it->second;
        }
        result[descriptors+i]=value;
    }
    const size_t data=result.size();result.resize(data+(bytes.size()+3)/4,0);
    for(size_t i=0;i<bytes.size();++i)result[data+i/4]|=uint32_t(bytes[i])<<((i&3u)*8u);
    return result;
}
}
