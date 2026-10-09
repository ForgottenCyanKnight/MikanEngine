#pragma once
#include "Rendering/VoxRenderer.h"
#include <map>
#include <algorithm>
#include <stdexcept>
#include <functional>
#include <array>

struct VoxContactBackfillStats {
    size_t candidates=0, emittedHidden=0, sourceReal=0, emittedReal=0;
};
using VoxContactQuery=std::function<bool(int,int,int,int)>;

// Bake planes are face coordinates; grid occupancy uses raw VOX x,y,z.
inline std::array<std::array<int,3>,2> VoxContactRawPair(
    int direction,int plane,int u,int v,int minX,int minY,int minZ) {
    const bool positive=direction==0||direction==3||direction==4;
    const int axis=direction<2?2:(direction<4?0:1);
    const int uAxis=direction<2?0:(direction<4?2:0),vAxis=direction<4?1:2;
    int local[3]{};local[axis]=plane-(positive?1:0);local[uAxis]=u;local[vAxis]=v;
    std::array<int,3> owner{minX+local[0],minY+local[2],minZ+local[1]};
    auto neighbor=owner;neighbor[direction<2?1:(direction<4?0:2)]+=positive?1:-1;
    return {owner,neighbor};
}

// Exterior surface reconstruction; optional contact cells extend rectangles
// inside a fixed opaque volume. Real surface attributes remain exact.
template<class Source>
inline void BakeVoxSurfaces(const Source& source, bool mergeColors,
    std::vector<VoxRtQuad>& output, std::vector<uint32_t>& materials,
    std::vector<VoxRayTracingRange>& ranges, std::vector<uint32_t>& attributes,
    const VoxContactQuery& contact={}, VoxContactBackfillStats* stats=nullptr,
    const VoxContactQuery& surfaceEligible={}) {
    if(stats)*stats={};
    // Group compact quads first; allocate only one bounded plane at a time.
    struct Face {int u,v,w,h;uint16_t palette;};
    std::map<std::pair<int,int>,std::vector<Face>> planes;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> paletteIds;
    std::vector<std::pair<uint32_t,uint32_t>> palette;
    const auto& input=source.GetQuads();const auto& words=source.GetQuadMaterials();
    if(input.size()!=words.size())throw std::runtime_error("VOX bake material count mismatch");
    for(const auto& group:source.GetMeshData().faceGroups) {
        for(uint32_t qi=uint32_t(group.firstIndex/6);qi<uint32_t((group.firstIndex+group.indexCount)/6);++qi) {
            const auto q=input[qi];int xyz[3]{int(q.geometry&255u),int((q.geometry>>8)&255u),int((q.geometry>>16)&255u)};
            const int d=group.faceDirection,axis=d<2?2:(d<4?0:1),uAxis=d<2?0:(d<4?2:0),vAxis=d<4?1:2;
            const int plane=xyz[axis]+((d==0||d==3||d==4)?1:0);
            const int w=int(q.geometry>>24)+1,h=int(q.appearance&255u)+1;
            const auto key=std::make_pair(q.appearance&0xffffff00u,words[qi]);
            auto [it,inserted]=paletteIds.emplace(key,uint32_t(palette.size()));
            if(inserted)palette.push_back(key);
            // Source VOX palettes are at most 256 entries. Reject rather than
            // silently truncate an index if a non-VOX producer exceeds that.
            if(palette.size()>256)throw std::runtime_error("VOX bake palette exceeds R8");
            planes[{d,plane}].push_back({xyz[uAxis],xyz[vAxis],w,h,uint16_t(it->second)});
        }
    }
    // Bit31: R8 byte offset; otherwise the low byte is a direct palette ID.
    // Width and length come from quad geometry, never duplicate them here.
    std::vector<uint32_t> descriptors;
    std::vector<uint8_t> bytes;std::map<std::vector<uint32_t>,uint32_t> blocks;
    size_t sourceArea=0,bakedArea=0,mixed=0,emittedReal=0,emittedHidden=0;
    for(const auto& [planeKey,faces]:planes) {
        const int direction=planeKey.first,plane=planeKey.second;
        int minU=256,minV=256,maxU=0,maxV=0;
        for(const auto& f:faces){minU=std::min(minU,f.u);minV=std::min(minV,f.v);maxU=std::max(maxU,f.u+f.w);maxV=std::max(maxV,f.v+f.h);}
        if(minU<0||minV<0||maxU>256||maxV>256)throw std::runtime_error("VOX bake plane exceeds encoding");
        const int stride=maxV-minV,extentU=maxU-minU;
        constexpr uint16_t empty=256,hidden=257;
        // u-major layout preserves the old map<pair<u,v>> traversal exactly.
        std::vector<uint16_t> cells(size_t(extentU)*stride,empty);
        auto cell=[&](int u,int v)->uint16_t&{return cells[size_t(u-minU)*stride+v-minV];};
        for(const auto& f:faces){
            sourceArea+=size_t(f.w)*f.h;
            for(int x=f.u;x<f.u+f.w;++x)for(int y=f.v;y<f.v+f.h;++y){
                auto& id=cell(x,y);if(id!=empty)throw std::runtime_error("Overlapping canonical VOX faces");id=f.palette;
            }
        }
        if(contact)for(int u=minU;u<maxU;++u)for(int v=minV;v<maxV;++v){
            if(cell(u,v)==empty&&contact(direction,plane,u,v)){
                cell(u,v)=hidden;if(stats)++stats->candidates;
            }
        }
        // Candidate audit retains immutable classification, including true holes.
        const std::vector<uint16_t> original=contact?cells:std::vector<uint16_t>{};
        std::vector<uint8_t> visits(contact?cells.size():0,0);
        if(ranges.empty()||ranges.back().direction!=uint32_t(direction))ranges.push_back({uint32_t(output.size()),0,uint32_t(direction)});
        for(size_t cursor=0;cursor<cells.size();++cursor) {
            if(cells[cursor]>=empty)continue; // A hidden-only rectangle must never seed.
            const int u=minU+int(cursor/stride),v=minV+int(cursor%stride);const uint32_t first=cells[cursor];
            const bool seedEligible=!(palette[first].second&2u)&&
                (!surfaceEligible||surfaceEligible(direction,plane,u,v));
            // Ordinary surfaces compare occupancy only. Emissive surfaces keep
            // uniform palette entries for the existing triangle light sampler.
            auto compatible=[&](int x,int y){
                if(x>=maxU||y>=maxV)return false;
                const uint32_t id=cell(x,y);if(id==empty)return false;
                if(id==hidden)return bool(contact)&&seedEligible;
                // A glass/ineligible surface is a local barrier, not an asset veto.
                if(contact&&surfaceEligible&&seedEligible!=
                    (!(palette[id].second&2u)&&surfaceEligible(direction,plane,x,y)))return false;
                return (!mergeColors||((palette[first].second|palette[id].second)&2u))?id==first:true;
            };
            int w=1,h=1;while(compatible(u+w,v))++w;
            for(;;){bool full=true;for(int x=0;x<w;++x)if(!compatible(u+x,v+h)){full=false;break;}if(!full)break;++h;}
            if(w>256||h>256)throw std::runtime_error("VOX bake quad extent exceeds encoding");
            std::vector<uint32_t> block{uint32_t(w),uint32_t(h)};bool solid=true;
            for(int y=0;y<h;++y)for(int x=0;x<w;++x){
                const uint16_t value=cell(u+x,v+y);
                if(value==empty)throw std::runtime_error("VOX backfill crossed a true hole");
                const uint32_t id=value==hidden?first:uint32_t(value);
                if(value==hidden)++emittedHidden;else ++emittedReal;
                if(contact){
                    const size_t index=size_t(u+x-minU)*stride+v+y-minV;
                    if(++visits[index]!=1||original[index]==empty||
                        (original[index]<empty&&original[index]!=id))
                        throw std::runtime_error("VOX backfill cell/attribute audit failed");
                }
                block.push_back(id);solid&=id==first;
            }
            uint32_t descriptor=first;
            if(!solid){auto [it,inserted]=blocks.emplace(block,uint32_t(bytes.size()));
                if(inserted)for(size_t i=2;i<block.size();++i)bytes.push_back(uint8_t(block[i]));
                if(it->second>=0x80000000u)throw std::runtime_error("VOX R8 offset exceeds descriptor");
                descriptor=0x80000000u|it->second;++mixed;}
            int xyz[3]{};const int axis=direction<2?2:(direction<4?0:1),uAxis=direction<2?0:(direction<4?2:0),vAxis=direction<4?1:2;
            xyz[axis]=plane-((direction==0||direction==3||direction==4)?1:0);xyz[uAxis]=u;xyz[vAxis]=v;
            for(int a:xyz)if(a<0||a>255)throw std::runtime_error("VOX bake origin exceeds encoding");
            output.push_back({uint32_t(xyz[0])|(uint32_t(xyz[1])<<8)|(uint32_t(xyz[2])<<16)|(uint32_t(w-1)<<24),uint32_t(h-1)|palette[first].first});
            materials.push_back(palette[first].second);descriptors.push_back(descriptor);++ranges.back().quadCount;
            for(int y=0;y<h;++y)for(int x=0;x<w;++x)cell(u+x,v+y)=empty;bakedArea+=size_t(w)*h;
        }
        if(contact)for(size_t i=0;i<original.size();++i)
            if((original[i]<empty&&visits[i]!=1)||(original[i]==empty&&visits[i]!=0))
                throw std::runtime_error("VOX backfill omitted or covered an invalid cell");
    }
    if(sourceArea!=emittedReal||bakedArea!=emittedReal+emittedHidden)
        throw std::runtime_error("VOX bake coverage mismatch");
    if(stats){stats->sourceReal=sourceArea;stats->emittedReal=emittedReal;stats->emittedHidden=emittedHidden;}
    // Header offsets are relative to this block; viewport relocates them once.
    attributes={4u,4u+uint32_t(palette.size())*2u,4u+uint32_t(palette.size())*2u+uint32_t(descriptors.size()),0u};
    for(auto p:palette){attributes.push_back(p.first);attributes.push_back(p.second);}
    attributes.insert(attributes.end(),descriptors.begin(),descriptors.end());
    const size_t offset=attributes.size();attributes.resize(offset+(bytes.size()+3)/4,0u);
    for(size_t i=0;i<bytes.size();++i)attributes[offset+i/4]|=uint32_t(bytes[i])<<((i%4)*8);
    // Verify every decoded attribute against the reconstructed source surface,
    // including dimensions and byte packing (geometry coverage verified above).
    for(const auto& [block,byteOffset]:blocks)for(size_t i=2;i<block.size();++i){const size_t b=byteOffset+i-2;
        if(((attributes[offset+b/4]>>((b%4)*8))&255u)!=block[i])throw std::runtime_error("VOX bake R8 roundtrip mismatch");}
    LOGI("[VOX surface merge] inputFaces=%zu sharedQuads=%zu solid=%zu mixed=%zu palette=%zu indexBytes=%zu attributeBytes=%zu coverage=%zu",
        input.size(),output.size(),output.size()-mixed,mixed,palette.size(),bytes.size(),attributes.size()*4,sourceArea);
}
