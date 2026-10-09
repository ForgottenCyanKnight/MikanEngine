#include "Rendering/RayTracing/RayTracingValidation.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "Core/Log.h"
#include "VoxSurfaceBake.h"
#include <set>
#include <random>
#include <iostream>

namespace VoxBackfillTests {
using Cell=std::array<int,3>;
using Key=std::array<int,4>; // direction, face plane, u, v
struct Source {
    std::vector<VoxRtQuad> quads;std::vector<uint32_t> materials;VoxelMeshData mesh{};
    const auto& GetQuads()const{return quads;}
    const auto& GetQuadMaterials()const{return materials;}
    const auto& GetMeshData()const{return mesh;}
};
struct Baked {std::vector<VoxRtQuad> quads;std::vector<uint32_t> materials,attributes;std::vector<VoxRayTracingRange> ranges;VoxContactBackfillStats stats;};
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
constexpr Cell normals[6]={{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
Key FaceKey(int d,Cell p){const int a=d<2?2:(d<4?0:1),u=d<2?0:(d<4?2:0),v=d<4?1:2;return {d,p[a]+(d==0||d==3||d==4),p[u],p[v]};}
std::pair<uint32_t,uint32_t> Attribute(const Baked& b,size_t q,int x,int y){
    const auto& a=b.attributes;const uint32_t descriptor=a[a[1]+uint32_t(q)],id=descriptor&0x7fffffffu;
    uint32_t entry=id;if(descriptor&0x80000000u){const uint32_t width=(b.quads[q].geometry>>24)+1u;
        const uint32_t byte=id+uint32_t(y)*width+uint32_t(x);entry=(a[a[2]+byte/4]>>((byte%4)*8))&255u;}
    return {a[a[0]+entry*2],a[a[0]+entry*2+1]};
}
void Fixture(const char* name,const std::set<Cell>& cells,bool multicolor,bool mustReduce,
    bool hasEmitter=false,bool hasGlass=false){
    Source source;std::map<Key,std::pair<uint32_t,uint32_t>> real;
    for(int d=0;d<6;++d){auto& group=source.mesh.faceGroups[d];group.faceDirection=d;group.firstIndex=source.quads.size()*6;
        for(auto p:cells){auto n=p;for(int i=0;i<3;++i)n[i]+=normals[d][i];if(cells.contains(n))continue;
            const uint32_t color=multicolor?uint32_t((p[0]+p[1]*3+p[2]*7)&255)<<8:0x12345600u;
            uint32_t material=multicolor?4u|(((color>>8)*19u&255u)<<8):0u;
            if(hasEmitter&&p[0]>=12)material=2u|(0x3c00u<<16);
            source.quads.push_back({uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16),color});source.materials.push_back(material);real.emplace(FaceKey(d,p),std::make_pair(color,material));
        }group.indexCount=source.quads.size()*6-group.firstIndex;
    }
    const VoxContactQuery contact=[&](int d,int plane,int u,int v){
        // Nonzero raw origin exercises normalization as well as the axis swap.
        auto pair=VoxContactRawPair(d,plane,u,v,11,17,23);
        for(auto raw:pair){Cell engine{raw[0]-11,raw[2]-23,raw[1]-17};
            if(!cells.contains(engine)||((hasEmitter||hasGlass)&&engine[0]>=12))return false;}return true;
    };
    const VoxContactQuery eligible=[&](int d,int plane,int u,int v){
        const auto owner=VoxContactRawPair(d,plane,u,v,11,17,23)[0];
        return !(hasEmitter||hasGlass)||owner[0]-11<12;
    };
    Baked baseline,candidate;
    BakeVoxSurfaces(source,true,baseline.quads,baseline.materials,baseline.ranges,baseline.attributes);
    BakeVoxSurfaces(source,true,candidate.quads,candidate.materials,candidate.ranges,candidate.attributes,contact,&candidate.stats,eligible);
    auto emitterSignature=[](const Baked& b){std::vector<std::array<uint32_t,4>> result;
        for(auto range:b.ranges)for(size_t q=range.firstQuad;q<range.firstQuad+range.quadCount;++q)
            if(b.materials[q]&2u)result.push_back({range.direction,b.quads[q].geometry,b.quads[q].appearance,b.materials[q]});
        std::sort(result.begin(),result.end());return result;
    };
    Require(emitterSignature(baseline)==emitterSignature(candidate),"Source emitter geometry/area/color changed");
    std::map<Key,int> visits;
    for(auto range:candidate.ranges)for(size_t q=range.firstQuad;q<range.firstQuad+range.quadCount;++q){
        const auto quad=candidate.quads[q];Cell p{int(quad.geometry&255),int((quad.geometry>>8)&255),int((quad.geometry>>16)&255)};
        auto key=FaceKey(range.direction,p);bool hasReal=false;
        for(int y=0;y<int((quad.appearance&255)+1);++y)for(int x=0;x<int((quad.geometry>>24)+1);++x){auto k=key;k[2]+=x;k[3]+=y;
            Require(++visits[k]==1,"Duplicate output cell");auto found=real.find(k);
            if(found!=real.end()){hasReal=true;Require(Attribute(candidate,q,x,y)==found->second,"Real RGB/material changed");}
            else Require(contact(k[0],k[1],k[2],k[3]),"True hole covered");
        }Require(hasReal,"Hidden-only quad");
    }
    for(const auto& [k,value]:real)Require(visits[k]==1,"Missing real cell");
    Require(candidate.stats.emittedReal==real.size(),"Real coverage mismatch");
    if(mustReduce)Require(candidate.quads.size()<baseline.quads.size(),"Expected contact reduction");
    if(std::string(name)=="plain"||std::string(name)=="window")Require(candidate.stats.emittedHidden==0,"Unexpected backfill");
    // External nearest-hit comparison: identical t, normal and sampled attributes.
    struct Hit {double t=1e30;int direction=-1;std::pair<uint32_t,uint32_t> attribute{};};
    auto trace=[&](const Baked& b,glm::dvec3 origin,glm::dvec3 direction){Hit hit;
        for(auto range:b.ranges)for(size_t q=range.firstQuad;q<range.firstQuad+range.quadCount;++q){const auto quad=b.quads[q];Cell p{int(quad.geometry&255),int((quad.geometry>>8)&255),int((quad.geometry>>16)&255)};
            const auto k=FaceKey(range.direction,p);const int d=range.direction,a=d<2?2:(d<4?0:1),u=d<2?0:(d<4?2:0),v=d<4?1:2;
            if(std::abs(direction[a])<1e-12)continue;const double t=(k[1]-origin[a])/direction[a];if(t<=0||t>=hit.t)continue;
            const auto point=origin+direction*t;const double x=point[u]-k[2],y=point[v]-k[3];const int w=int(quad.geometry>>24)+1,h=int(quad.appearance&255)+1;
            if(x<0||y<0||x>=w||y>=h)continue;hit={t,d,Attribute(b,q,int(x),int(y))};
        }return hit;
    };
    if(!cells.empty()){
        Cell hi{};for(auto p:cells)for(int a=0;a<3;++a)hi[a]=std::max(hi[a],p[a]+1);
        std::mt19937 random(20261009);std::uniform_real_distribution<double> unit(.001,.999);
        for(int i=0;i<3000;++i){glm::dvec3 origin,target;for(int a=0;a<3;++a){origin[a]=unit(random)*hi[a];target[a]=unit(random)*hi[a];}
            const int axis=i%3;origin[axis]=(i&1)?hi[axis]+10.0:-10.0;const auto direction=glm::normalize(target-origin);
            const auto a=trace(baseline,origin,direction),b=trace(candidate,origin,direction);
            Require(std::abs(a.t-b.t)<1e-8&&a.direction==b.direction&&a.attribute==b.attribute,"External nearest-hit mismatch");
        }
    }
    std::cout<<"PASS "<<name<<" baseline="<<baseline.quads.size()<<" candidate="<<candidate.quads.size()<<" hidden="<<candidate.stats.emittedHidden<<" rays="<<(cells.empty()?0:3000)<<'\n';
}
int CpuBackfillTests(){try{
    for(int axis=0;axis<3;++axis)for(int sign:{-1,1})for(bool mixed:{false,true}){
        std::set<Cell> cells;for(int u=0;u<8;++u)for(int v=0;v<8;++v){Cell p{};p[axis]=sign>0?0:2;p[(axis+1)%3]=u;p[(axis+2)%3]=v;cells.insert(p);}
        for(int layer=1;layer<=2;++layer)for(int u=3;u<5;++u)for(int v=3;v<5;++v){Cell p{};p[axis]=sign>0?layer:2-layer;p[(axis+1)%3]=u;p[(axis+2)%3]=v;cells.insert(p);}
        Fixture(mixed?"contact_multicolor":"contact_six_directions",cells,mixed,true);
    }
    std::set<Cell> floor;for(int x=0;x<8;++x)for(int z=0;z<8;++z)floor.insert({x,0,z});Fixture("plain",floor,false,false);
    for(int x=3;x<5;++x)for(int z=3;z<5;++z)floor.erase({x,0,z});Fixture("window",floor,true,false);
    floor.clear();for(int x=0;x<256;++x)for(int z=0;z<2;++z)floor.insert({x,255,z});Fixture("boundary_256",floor,true,false);
    Fixture("no_real_plane",{},false,false);
    std::set<Cell> room;
    for(int x=0;x<8;++x)for(int z=0;z<8;++z)room.insert({x,0,z});
    for(int y=1;y<3;++y)for(int x=3;x<5;++x)for(int z=3;z<5;++z)room.insert({x,y,z});
    // A second contact surface is emissive/glass. It must stay unfilled while
    // the ordinary floor in the SAME source still benefits from backfill.
    for(int x=12;x<20;++x)for(int z=0;z<8;++z)room.insert({x,0,z});
    for(int y=1;y<3;++y)for(int x=15;x<17;++x)for(int z=3;z<5;++z)room.insert({x,y,z});
    Fixture("ordinary_floor_with_emissive_contact",room,true,true,true,false);
    Fixture("ordinary_floor_with_glass_contact",room,true,true,false,true);
    std::cout<<"CPU contact backfill fixtures passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
