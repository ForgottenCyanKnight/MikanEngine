#pragma once
#include "Rendering/VoxRenderer.h"
#include "Rendering/VoxLoadLog.h"
#include "VoxRectangleDecomposition.h"
#include <map>
#include <algorithm>
#include <stdexcept>
#include <functional>
#include <array>

struct VoxContactBackfillStats {
    size_t candidates=0, emittedHidden=0, sourceReal=0, emittedReal=0;
    size_t baselineQuads=0;
};
using VoxContactQuery=std::function<bool(int,int,int,int)>;
enum class VoxSurfacePartitionMethod { Greedy, RectangleDecomposition };

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

// Plane data and rectangle plans are shared by baseline and contact baking.
// Dense cells live for one plane only; retained hidden coverage uses one bit/cell.
namespace VoxSurfaceBakeDetail {
constexpr uint16_t Empty=65535,Hidden=65534;
struct Face {int u,v,w,h;uint16_t palette;};
struct Rect {int u,v,w,h;uint16_t first;};
struct Plane {
    int direction,position,minU,minV,maxU,maxV;
    std::vector<Face> faces;
    int Stride()const{return maxV-minV;}
    int ExtentU()const{return maxU-minU;}
};
struct Prepared {
    std::vector<Plane> planes;
    std::vector<std::pair<uint32_t,uint32_t>> palette;
    size_t sourceArea=0,inputQuads=0;
};
struct PlanePlan {std::vector<Rect> rectangles;std::vector<uint64_t> hidden;};
struct Plan {
    std::vector<PlanePlan> planes;
    size_t quadCount=0;
    VoxContactBackfillStats stats;
};
template<class Source>
inline Prepared Prepare(const Source& source){
    Prepared result;
    std::map<std::pair<int,int>,std::vector<Face>> planes;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> paletteIds;
    const auto& input=source.GetQuads();const auto& words=source.GetQuadMaterials();
    result.inputQuads=input.size();
    if(input.size()!=words.size())throw std::runtime_error("VOX bake material count mismatch");
    for(const auto& group:source.GetMeshData().faceGroups){
        for(uint32_t qi=uint32_t(group.firstIndex/6);qi<uint32_t((group.firstIndex+group.indexCount)/6);++qi){
            const auto q=input[qi];int xyz[3]{int(q.geometry&255u),int((q.geometry>>8)&255u),int((q.geometry>>16)&255u)};
            const int d=group.faceDirection,axis=d<2?2:(d<4?0:1),uAxis=d<2?0:(d<4?2:0),vAxis=d<4?1:2;
            const int plane=xyz[axis]+((d==0||d==3||d==4)?1:0);
            const int w=int(q.geometry>>24)+1,h=int(q.appearance&255u)+1;
            const auto key=std::make_pair(q.appearance&0xffffff00u,words[qi]);
            auto [it,inserted]=paletteIds.emplace(key,uint32_t(result.palette.size()));
            if(inserted)result.palette.push_back(key);
            if(result.palette.size()>65534)throw std::runtime_error("Voxel bake palette exceeds R16");
            planes[{d,plane}].push_back({xyz[uAxis],xyz[vAxis],w,h,uint16_t(it->second)});
            result.sourceArea+=size_t(w)*h;
        }
    }
    result.planes.reserve(planes.size());
    for(auto& [key,faces]:planes){
        Plane plane{key.first,key.second,256,256,0,0,std::move(faces)};
        for(const auto& f:plane.faces){plane.minU=std::min(plane.minU,f.u);plane.minV=std::min(plane.minV,f.v);plane.maxU=std::max(plane.maxU,f.u+f.w);plane.maxV=std::max(plane.maxV,f.v+f.h);}
        if(plane.minU<0||plane.minV<0||plane.maxU>256||plane.maxV>256)throw std::runtime_error("VOX bake plane exceeds encoding");
        result.planes.push_back(std::move(plane));
    }
    return result;
}
inline std::vector<uint16_t> RealCells(const Plane& plane){
    const int stride=plane.Stride();
    std::vector<uint16_t> cells(size_t(plane.ExtentU())*stride,Empty);
    for(const auto& f:plane.faces)for(int u=f.u;u<f.u+f.w;++u)for(int v=f.v;v<f.v+f.h;++v){
        auto& id=cells[size_t(u-plane.minU)*stride+v-plane.minV];
        if(id!=Empty)throw std::runtime_error("Overlapping canonical VOX faces");
        id=f.palette;
    }
    return cells;
}
// If pureBaseline is supplied, build both plans on the same prepared plane.
// Only the eligibility-sensitive contact baseline needs its own partition.
inline Plan Partition(const Prepared& prepared,bool mergeColors,const VoxContactQuery& contact,
    const VoxContactQuery& surfaceEligible,bool searchOrientations,VoxSurfacePartitionMethod method,
    Plan* pureBaseline=nullptr){
    Plan result;result.planes.reserve(prepared.planes.size());
    if(pureBaseline){*pureBaseline={};pureBaseline->planes.reserve(prepared.planes.size());}
    const auto& palette=prepared.palette;
    mikan::vox::RectangleDecomposer rectangleDecomposer;
    std::vector<uint32_t> mergeLabels;
    for(const auto& planeData:prepared.planes){
        const auto& faces=planeData.faces;
        const int direction=planeData.direction,plane=planeData.position;
        const int minU=planeData.minU,minV=planeData.minV,maxU=planeData.maxU,maxV=planeData.maxV;
        const int stride=planeData.Stride(),extentU=planeData.ExtentU();
        constexpr uint16_t empty=Empty,hidden=Hidden;
        auto cells=RealCells(planeData);
        auto cell=[&](int u,int v)->uint16_t&{return cells[size_t(u-minU)*stride+v-minV];};
        std::vector<uint8_t> eligibleCells;
        bool uniformEligibility=true;int firstEligibility=-1;
        if(contact&&surfaceEligible){
            eligibleCells.resize(cells.size());
            // Query source eligibility once, before any orientation consumes cells.
            for(const auto& f:faces)for(int u=f.u;u<f.u+f.w;++u)for(int v=f.v;v<f.v+f.h;++v){
                const bool eligible=!(palette[f.palette].second&2u)&&surfaceEligible(direction,plane,u,v);
                eligibleCells[size_t(u-minU)*stride+v-minV]=uint8_t(eligible);
                if(firstEligibility<0)firstEligibility=int(eligible);
                else if(firstEligibility!=int(eligible))uniformEligibility=false;
            }
        }
        bool useEligibilityBarrier=bool(contact&&surfaceEligible);
        auto greedyPartition=[&](const std::vector<uint16_t>& inputCells,bool allowHidden,bool testOrientations){
            std::vector<Rect> best;
            const bool hasEmitter=std::any_of(faces.begin(),faces.end(),[&](const Face& face){return palette[face.palette].second&2u;});
            // Keep the existing emitter tessellation byte-for-byte: the light
            // sampler treats each source quad as a separately weighted light.
            // This D4 search is a bounded greedy improvement, not a global optimum proof.
            const int transformCount=testOrientations&&!hasEmitter?8:1;
            for(int transform=0;transform<transformCount;++transform){
                if(best.size()==1)break; // One rectangle is the nonempty lower bound.
                const bool swap=(transform&1)!=0,flipU=(transform&2)!=0,flipV=(transform&4)!=0;
                const int scanU=swap?stride:extentU,scanV=swap?extentU:stride;
                auto mapCell=[&](int i,int j){
                    if(flipU)i=scanU-1-i;
                    if(flipV)j=scanV-1-j;
                    return swap?std::pair<int,int>{j,i}:std::pair<int,int>{i,j};
                };
                auto indexOf=[&](int u,int v){return size_t(u)*stride+v;};
                auto read=[&](const std::vector<uint16_t>& grid,int i,int j){const auto [u,v]=mapCell(i,j);return grid[indexOf(u,v)];};
                std::vector<uint16_t> remaining=inputCells;
                std::vector<Rect> candidate;
                bool cannotWin=false;
                for(int i=0;i<scanU&&!cannotWin;++i)for(int j=0;j<scanV;++j){
                    const uint16_t first=read(remaining,i,j);
                    if(first==empty||first==hidden)continue; // Hidden cells never seed.
                    const auto [seedU,seedV]=mapCell(i,j);
                    const bool seedEligible=!(palette[first].second&2u)&&
                        (!useEligibilityBarrier||eligibleCells[size_t(seedU)*stride+seedV]);
                    auto compatible=[&](int x,int y){
                        if(x<0||y<0||x>=scanU||y>=scanV)return false;
                        const auto [u,v]=mapCell(x,y);const uint32_t id=remaining[indexOf(u,v)];
                        if(id==empty)return false;
                        if(id==hidden)return allowHidden&&bool(contact)&&seedEligible;
                        // A glass/ineligible surface is a local barrier, not an asset veto.
                        if(useEligibilityBarrier&&seedEligible!=bool(eligibleCells[indexOf(u,v)]))return false;
                        return (!mergeColors||((palette[first].second|palette[id].second)&2u))?id==first:true;
                    };
                    int w=1,h=1;while(compatible(i+w,j))++w;
                    for(;;){bool full=true;for(int x=0;x<w;++x)if(!compatible(i+x,j+h)){full=false;break;}if(!full)break;++h;}
                    int outMinU=extentU,outMinV=stride,outMaxU=-1,outMaxV=-1;
                    const std::array<std::pair<int,int>,4> corners{{{i,j},{i+w-1,j},{i,j+h-1},{i+w-1,j+h-1}}};
                    for(const auto& point:corners){
                        const auto [u,v]=mapCell(point.first,point.second);
                        outMinU=std::min(outMinU,u);outMinV=std::min(outMinV,v);outMaxU=std::max(outMaxU,u);outMaxV=std::max(outMaxV,v);
                    }
                    candidate.push_back({outMinU,outMinV,outMaxU-outMinU+1,outMaxV-outMinV+1,first});
                    // Ties retain the earlier orientation, so this candidate can
                    // no longer win even before its remaining cells are scanned.
                    if(!best.empty()&&candidate.size()>=best.size()){cannotWin=true;break;}
                    for(int x=0;x<w;++x)for(int y=0;y<h;++y){const auto [u,v]=mapCell(i+x,j+y);remaining[indexOf(u,v)]=empty;}
                }
                if(!cannotWin&&(best.empty()||candidate.size()<best.size()))best=std::move(candidate);
            }
            return best;
        };

        auto partition=[&](const std::vector<uint16_t>& inputCells,bool allowHidden,bool testOrientations){
            if(method==VoxSurfacePartitionMethod::Greedy)return greedyPartition(inputCells,allowHidden,testOrientations);
            // Hidden contact cells are optional wildcards, not ordinary labels.
            // Retain the existing extension search for that separate problem;
            // its result must still beat the new exact exposed-surface baseline.
            if(allowHidden&&contact)return greedyPartition(inputCells,allowHidden,testOrientations);
            mergeLabels.assign(size_t(extentU)*stride,0);
            bool hasEmitter=false;
            for(int u=0;u<extentU;++u)for(int v=0;v<stride;++v){
                const auto id=inputCells[size_t(u)*stride+v];
                if(id==empty||id==hidden)continue;
                if(palette[id].second&2u){hasEmitter=true;continue;}
                uint32_t label=mergeColors?65535u:uint32_t(id)+1u;
                if(useEligibilityBarrier)
                    label=label*2u+uint32_t(eligibleCells[size_t(u)*stride+v]);
                // The solver is row-major (x=u,y=v); the baker stays u-major.
                mergeLabels[size_t(v)*extentU+u]=label;
            }
            const auto decomposition=rectangleDecomposer.Decompose(mergeLabels,extentU,stride);
            std::vector<Rect> result;result.reserve(decomposition.size());
            for(const auto& rect:decomposition)
                result.push_back({rect.x,rect.y,rect.width,rect.height,inputCells[size_t(rect.x)*stride+rect.y]});
            if(hasEmitter){
                // Source-emitter quads define light sampling distributions.
                // Do not silently retessellate them while optimizing other cells.
                auto emitterCells=inputCells;
                for(auto& id:emitterCells)
                    if(id==empty||id==hidden||!(palette[id].second&2u))id=empty;
                auto emitters=greedyPartition(emitterCells,false,false);
                result.insert(result.end(),emitters.begin(),emitters.end());
            }
            std::sort(result.begin(),result.end(),[](const Rect& a,const Rect& b){
                return std::pair{a.u,a.v}<std::pair{b.u,b.v};
            });
            return result;
        };


        std::vector<Rect> baselineRects;
        if(pureBaseline){
            useEligibilityBarrier=false;
            auto pureRects=partition(cells,false,false);
            pureBaseline->quadCount+=pureRects.size();
            pureBaseline->planes.push_back({std::move(pureRects),{}});
            useEligibilityBarrier=bool(contact&&surfaceEligible);
            if(!useEligibilityBarrier||uniformEligibility)baselineRects=pureBaseline->planes.back().rectangles;
            else baselineRects=partition(cells,false,false);
        }else if(contact&&searchOrientations)baselineRects=partition(cells,false,false);
        if(contact)for(int u=minU;u<maxU;++u)for(int v=minV;v<maxV;++v){
            if(cell(u,v)==empty&&contact(direction,plane,u,v)){cell(u,v)=hidden;++result.stats.candidates;}
        }
        PlanePlan selected;
        if(contact&&searchOrientations){
            result.stats.baselineQuads+=baselineRects.size();
            if(baselineRects.size()>1)selected.rectangles=partition(cells,true,true);
            else selected.rectangles=baselineRects;
            if(selected.rectangles.size()>=baselineRects.size()){
                selected.rectangles=std::move(baselineRects);
                for(auto& id:cells)if(id==hidden)id=empty;
            }
        }else{
            selected.rectangles=partition(cells,bool(contact),false);
            result.stats.baselineQuads+=selected.rectangles.size();
        }
        // Validate the chosen candidate before discarding the dense plane.
        // Keep just hidden coverage bits for the eventual attribute pack.
        std::vector<uint8_t> visits(cells.size(),0);
        for(const auto& rect:selected.rectangles)for(int x=0;x<rect.w;++x)for(int y=0;y<rect.h;++y){
            const size_t index=size_t(rect.u+x)*stride+rect.v+y;const auto id=cells[index];
            if(++visits[index]!=1||id==empty)throw std::runtime_error("VOX backfill cell/attribute audit failed");
            if(id==hidden){
                if(selected.hidden.empty())selected.hidden.resize((cells.size()+63)/64);
                selected.hidden[index/64]|=uint64_t(1)<<(index%64);++result.stats.emittedHidden;
            }else ++result.stats.emittedReal;
        }
        for(size_t i=0;i<cells.size();++i)
            if((cells[i]<hidden&&visits[i]!=1)||(cells[i]==empty&&visits[i]!=0))
                throw std::runtime_error("VOX backfill omitted or covered an invalid cell");
        result.quadCount+=selected.rectangles.size();result.planes.push_back(std::move(selected));
    }
    result.stats.sourceReal=prepared.sourceArea;
    if(result.stats.emittedReal!=prepared.sourceArea)throw std::runtime_error("VOX bake coverage mismatch");
    if(pureBaseline){pureBaseline->stats.sourceReal=pureBaseline->stats.emittedReal=prepared.sourceArea;pureBaseline->stats.baselineQuads=pureBaseline->quadCount;}
    return result;
}
inline VoxRtQuad Encode(const Plane& plane,const Rect& rect,uint32_t color){
    const int d=plane.direction,u=plane.minU+rect.u,v=plane.minV+rect.v;
    if(rect.w<1||rect.h<1||rect.w>256||rect.h>256)throw std::runtime_error("VOX bake quad extent exceeds encoding");
    int xyz[3]{};const int axis=d<2?2:(d<4?0:1),ua=d<2?0:(d<4?2:0),va=d<4?1:2;
    xyz[axis]=plane.position-((d==0||d==3||d==4)?1:0);xyz[ua]=u;xyz[va]=v;
    for(int a:xyz)if(a<0||a>255)throw std::runtime_error("VOX bake origin exceeds encoding");
    return {uint32_t(xyz[0])|(uint32_t(xyz[1])<<8)|(uint32_t(xyz[2])<<16)|(uint32_t(rect.w-1)<<24),uint32_t(rect.h-1)|color};
}
inline std::vector<std::array<uint32_t,4>> Emitters(const Prepared& prepared,const Plan& plan){
    std::vector<std::array<uint32_t,4>> result;
    for(size_t i=0;i<prepared.planes.size();++i)for(const auto& rect:plan.planes[i].rectangles){
        const auto [color,material]=prepared.palette[rect.first];
        if(material&2u){const auto q=Encode(prepared.planes[i],rect,color);result.push_back({uint32_t(prepared.planes[i].direction),q.geometry,q.appearance,material});}
    }
    std::sort(result.begin(),result.end());return result;
}
// Serialize only the plan that will actually be used. Packing needs source
// colors plus saved hidden bits, and never repeats raw occupancy queries.
inline void Pack(const Prepared& prepared,const Plan& plan,std::vector<VoxRtQuad>& output,
    std::vector<uint32_t>& materials,std::vector<VoxRayTracingRange>& ranges,std::vector<uint32_t>& attributes){
    const auto& palette=prepared.palette;const bool wide=palette.size()>256;const size_t indexStride=wide?2u:1u;
    std::vector<uint32_t> descriptors;
    std::vector<uint8_t> bytes;std::map<std::vector<uint32_t>,uint32_t> blocks;
    size_t mixed=0,emittedReal=0,emittedHidden=0;
    for(size_t pi=0;pi<prepared.planes.size();++pi){
        const auto& plane=prepared.planes[pi];const auto& selected=plan.planes[pi];
        const int stride=plane.Stride();const auto cells=RealCells(plane);
        if(ranges.empty()||ranges.back().direction!=uint32_t(plane.direction))ranges.push_back({uint32_t(output.size()),0,uint32_t(plane.direction)});
        std::vector<uint8_t> visits(cells.size());
        for(const auto& rect:selected.rectangles){
            const auto first=rect.first;std::vector<uint32_t> block{uint32_t(rect.w),uint32_t(rect.h)};bool solid=true;
            block.reserve(size_t(rect.w)*rect.h+2);
            for(int y=0;y<rect.h;++y)for(int x=0;x<rect.w;++x){
                const size_t index=size_t(rect.u+x)*stride+rect.v+y;const auto value=cells[index];
                const bool hidden=!selected.hidden.empty()&&((selected.hidden[index/64]>>(index%64))&1u);
                if(++visits[index]!=1||(value==Empty&&!hidden)||(value!=Empty&&hidden))throw std::runtime_error("VOX packed plan coverage mismatch");
                const uint32_t id=hidden?first:uint32_t(value);
                if(hidden)++emittedHidden;else ++emittedReal;
                block.push_back(id);solid&=id==first;
            }
            uint32_t descriptor=first;
            if(!solid){auto [it,inserted]=blocks.emplace(block,uint32_t(bytes.size()));
                if(inserted)for(size_t i=2;i<block.size();++i){bytes.push_back(uint8_t(block[i]));if(wide)bytes.push_back(uint8_t(block[i]>>8));}
                if(it->second>=0x40000000u)throw std::runtime_error("Voxel index offset exceeds descriptor");
                descriptor=(wide?0xc0000000u:0x80000000u)|it->second;++mixed;}
            output.push_back(Encode(plane,rect,palette[first].first));
            materials.push_back(palette[first].second);descriptors.push_back(descriptor);++ranges.back().quadCount;
        }
        for(size_t i=0;i<cells.size();++i)if(cells[i]!=Empty&&visits[i]!=1)throw std::runtime_error("VOX packed plan omitted source cell");
    }
    if(emittedReal!=prepared.sourceArea||emittedHidden!=plan.stats.emittedHidden)throw std::runtime_error("VOX bake coverage mismatch");
    attributes={4u,4u+uint32_t(palette.size())*2u,4u+uint32_t(palette.size())*2u+uint32_t(descriptors.size()),0u};
    for(auto p:palette){attributes.push_back(p.first);attributes.push_back(p.second);}
    attributes.insert(attributes.end(),descriptors.begin(),descriptors.end());
    const size_t offset=attributes.size();attributes.resize(offset+(bytes.size()+3)/4,0u);
    for(size_t i=0;i<bytes.size();++i)attributes[offset+i/4]|=uint32_t(bytes[i])<<((i%4)*8);
    for(const auto& [block,byteOffset]:blocks)for(size_t i=2;i<block.size();++i){const size_t b=byteOffset+(i-2)*indexStride;
        if(((attributes[offset+b/4]>>((b%4)*8))&(wide?65535u:255u))!=block[i])throw std::runtime_error("Voxel index roundtrip mismatch");}
    if(VoxLoadVerbose())LOGI("[VOX surface merge] inputFaces=%zu sharedQuads=%zu solid=%zu mixed=%zu palette=%zu indexBytes=%zu attributeBytes=%zu coverage=%zu",
        prepared.inputQuads,output.size(),output.size()-mixed,mixed,palette.size(),bytes.size(),attributes.size()*4,prepared.sourceArea);
}
} // namespace VoxSurfaceBakeDetail

// Existing entry point retains its exact baseline/contact selection semantics.
template<class Source>
inline void BakeVoxSurfaces(const Source& source,bool mergeColors,
    std::vector<VoxRtQuad>& output,std::vector<uint32_t>& materials,
    std::vector<VoxRayTracingRange>& ranges,std::vector<uint32_t>& attributes,
    const VoxContactQuery& contact={},VoxContactBackfillStats* stats=nullptr,
    const VoxContactQuery& surfaceEligible={},bool searchOrientations=false,
    VoxSurfacePartitionMethod method=VoxSurfacePartitionMethod::Greedy){
    const auto prepared=VoxSurfaceBakeDetail::Prepare(source);
    const auto plan=VoxSurfaceBakeDetail::Partition(prepared,mergeColors,contact,surfaceEligible,searchOrientations,method);
    VoxSurfaceBakeDetail::Pack(prepared,plan,output,materials,ranges,attributes);
    if(stats)*stats=plan.stats;
}
struct VoxSurfaceBakeSelection {
    size_t baselineQuads=0,candidateQuads=0,emissiveQuads=0;
    bool accepted=false;
    VoxContactBackfillStats contact;
};
// Same model-wide best-of-baseline/contact rule as VoxRenderer, but preparation
// and ordinary baseline rectangles are shared, and attributes are packed once.
template<class Source>
inline VoxSurfaceBakeSelection BakeVoxSurfacesWithBackfill(const Source& source,bool mergeColors,
    std::vector<VoxRtQuad>& output,std::vector<uint32_t>& materials,
    std::vector<VoxRayTracingRange>& ranges,std::vector<uint32_t>& attributes,
    const VoxContactQuery& contact,const VoxContactQuery& surfaceEligible={},
    VoxSurfacePartitionMethod method=VoxSurfacePartitionMethod::Greedy){
    const auto prepared=VoxSurfaceBakeDetail::Prepare(source);
    VoxSurfaceBakeDetail::Plan baseline;
    const auto candidate=VoxSurfaceBakeDetail::Partition(prepared,mergeColors,contact,surfaceEligible,true,method,&baseline);
    const auto emitters=VoxSurfaceBakeDetail::Emitters(prepared,baseline);
    if(emitters!=VoxSurfaceBakeDetail::Emitters(prepared,candidate))throw std::runtime_error("VOX contact backfill changed source-emitter geometry or attributes");
    const bool accepted=candidate.quadCount<baseline.quadCount;
    VoxSurfaceBakeDetail::Pack(prepared,accepted?candidate:baseline,output,materials,ranges,attributes);
    return {baseline.quadCount,candidate.quadCount,emitters.size(),accepted,candidate.stats};
}
