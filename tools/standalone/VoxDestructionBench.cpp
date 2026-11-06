// CPU-only timings of production surface baking and experimental chunk rebuilds.
// Chunk tests rebuild owned cells with occupancy queries into the resident model
// (the equivalent of a one-cell halo). They do not create a runtime destruction system.
#include "Core/Log.h"
#include "Rendering/VoxFaceMaterial.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "Rendering/RayTracing/VoxSurfaceBake.h"
#include "json.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <cmath>

namespace VoxDestructionBench {
using Clock=std::chrono::steady_clock;
using Cell=std::array<int,3>;
constexpr Cell Normals[6]={{0,1,0},{0,-1,0},{-1,0,0},{1,0,0},{0,0,1},{0,0,-1}};
double Ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
struct Box {Cell lo{},hi{};bool Contains(Cell p)const{return p[0]>=lo[0]&&p[1]>=lo[1]&&p[2]>=lo[2]&&p[0]<hi[0]&&p[1]<hi[1]&&p[2]<hi[2];}};
struct Source {
 std::vector<VoxRtQuad> quads;std::vector<uint32_t> materials;VoxelMeshData mesh{};
 const auto& GetQuads()const{return quads;} const auto& GetQuadMaterials()const{return materials;}
 const auto& GetMeshData()const{return mesh;}
};
struct Grid {
 const VoxFormat::VoxData& data;const VoxFormat::Model& model;Box bounds{{256,256,256},{0,0,0}};
 int sx=0,sy=0,sz=0;std::vector<uint8_t> cells;
 Grid(const VoxFormat::VoxData& d,const VoxFormat::Model& m):data(d),model(m){
  for(const auto& v:m.voxels){Cell p{v.x,v.y,v.z};for(int a=0;a<3;++a){bounds.lo[a]=std::min(bounds.lo[a],p[a]);bounds.hi[a]=std::max(bounds.hi[a],p[a]+1);}}
  if(m.voxels.empty()){bounds={};return;}
  sx=bounds.hi[0];sy=bounds.hi[1];sz=bounds.hi[2];cells.resize(size_t(sx)*sy*sz);
  for(const auto& v:m.voxels)Set({v.x,v.y,v.z},v.colorIndex);
 }
 uint8_t Get(Cell p)const{return p[0]<0||p[1]<0||p[2]<0||p[0]>=sx||p[1]>=sy||p[2]>=sz?0:cells[(size_t(p[2])*sy+p[1])*sx+p[0]];}
 void Set(Cell p,uint8_t id){cells[(size_t(p[2])*sy+p[1])*sx+p[0]]=id;}
 bool Ordinary(Cell p)const{const auto id=Get(p);return id&&(!data.hasMaterials||(data.materials[id].type!=2&&data.materials[id].type!=3));}
};
Source Extract(const Grid& grid,const Box& box,bool full){
 Source source;
 for(int d=0;d<6;++d){auto& group=source.mesh.faceGroups[d];group.faceDirection=d;group.firstIndex=source.quads.size()*6;
  auto emit=[&](Cell p,uint8_t id){
   if(!id)return;auto n=p;for(int a=0;a<3;++a)n[a]+=Normals[d][a];if(grid.Get(n))return;
   const auto c=grid.data.palette[id];const uint32_t x=p[0]-box.lo[0],y=p[2]-box.lo[2],z=p[1]-box.lo[1];
   source.quads.push_back({x|(y<<8)|(z<<16),(uint32_t(c.r)<<8)|(uint32_t(c.g)<<16)|(uint32_t(c.b)<<24)});
   source.materials.push_back(PackVoxFaceMaterial(grid.data,id));
  };
  if(full){for(const auto& v:grid.model.voxels){Cell p{v.x,v.y,v.z};emit(p,grid.Get(p));}}
  else for(int z=box.lo[2];z<box.hi[2];++z)for(int y=box.lo[1];y<box.hi[1];++y)for(int x=box.lo[0];x<box.hi[0];++x){Cell p{x,y,z};emit(p,grid.Get(p));}
  group.indexCount=source.quads.size()*6-group.firstIndex;
 }
 return source;
}
struct BakeResult {double ms=0;size_t quads=0,attributes=0,hidden=0;};
BakeResult Bake(const Source& source,const Grid& grid,const Box& box,bool backfill,VoxSurfacePartitionMethod method=VoxSurfacePartitionMethod::Greedy){
 const VoxContactQuery contact=[&](int d,int p,int u,int v){const auto pair=VoxContactRawPair(d,p,u,v,box.lo[0],box.lo[1],box.lo[2]);return box.Contains(pair[0])&&grid.Ordinary(pair[0])&&grid.Ordinary(pair[1]);};
 const VoxContactQuery eligible=[&](int d,int p,int u,int v){return grid.Ordinary(VoxContactRawPair(d,p,u,v,box.lo[0],box.lo[1],box.lo[2])[0]);};
 std::vector<VoxRtQuad> q;std::vector<uint32_t> m,a;std::vector<VoxRayTracingRange> r;
 const auto start=Clock::now();size_t hidden=0;
 if(backfill){const auto result=BakeVoxSurfacesWithBackfill(source,true,q,m,r,a,contact,eligible,method);hidden=result.accepted?result.contact.emittedHidden:0;}
 else BakeVoxSurfaces(source,true,q,m,r,a,{},nullptr,{},false,method);
 return {Ms(start),q.size(),a.size()*4,hidden};
}
nlohmann::json Distribution(std::vector<double> values){
 if(values.empty())return {{"samples",0},{"median_ms",0},{"p95_ms",0},{"max_ms",0}};
 std::sort(values.begin(),values.end());const size_t n=values.size();double median=n%2?values[n/2]:(values[n/2-1]+values[n/2])*.5;
 return {{"samples",n},{"median_ms",median},{"p95_ms",values[size_t(std::ceil(n*.95))-1]},{"max_ms",values.back()}};
}
nlohmann::json Counts(const std::vector<double>& values){
 const auto d=Distribution(values);return {{"samples",d["samples"]},{"median",d["median_ms"]},{"p95",d["p95_ms"]},{"max",d["max_ms"]}};
}
Box ChunkBox(const Grid& grid,Cell key,int edge){Box box;for(int a=0;a<3;++a){box.lo[a]=key[a]*edge;box.hi[a]=std::min(box.lo[a]+edge,grid.bounds.hi[a]);}return box;}
std::set<Cell> AllChunks(const Grid& grid,int edge){std::set<Cell> result;for(auto v:grid.model.voxels)result.insert({v.x/edge,v.y/edge,v.z/edge});return result;}
struct Removed {Cell p;uint8_t id;};
std::vector<Removed> Damage(Grid& grid,Cell center,int radius){
 std::vector<Removed> removed;
 for(int dz=-radius;dz<=radius;++dz)for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx){
  if(dx*dx+dy*dy+dz*dz>radius*radius)continue;Cell p{center[0]+dx,center[1]+dy,center[2]+dz};auto id=grid.Get(p);if(id){removed.push_back({p,id});grid.Set(p,0);}
 }
 return removed;
}
std::set<Cell> DirtyChunks(const Grid& grid,const std::vector<Removed>& removed,int edge){
 std::set<Cell> dirty;
 for(auto v:removed)for(int direction=-1;direction<6;++direction){auto p=v.p;if(direction>=0)for(int a=0;a<3;++a)p[a]+=Normals[direction][a];
  if(p[0]>=0&&p[1]>=0&&p[2]>=0&&p[0]<grid.sx&&p[1]<grid.sy&&p[2]<grid.sz)dirty.insert({p[0]/edge,p[1]/edge,p[2]/edge});}
 return dirty;
}
// Verify the halo extractor independently: all chunk source faces must be exactly
// the original model's source faces, expressed in global owner coordinates.
using FaceKey=std::array<int,4>;
std::set<FaceKey> FaceKeys(const Source& source,Cell origin){
 std::set<FaceKey> keys;
 for(const auto& g:source.mesh.faceGroups)for(size_t i=g.firstIndex/6;i<(g.firstIndex+g.indexCount)/6;++i){auto q=source.quads[i];
  if(!keys.insert({int(g.faceDirection),int(q.geometry&255)+origin[0],int((q.geometry>>16)&255)+origin[1],int((q.geometry>>8)&255)+origin[2]}).second)throw std::runtime_error("duplicate source face");}
 return keys;
}
nlohmann::json BenchmarkModel(const VoxFormat::VoxData& data,uint32_t index,int repeats,int edits){
 const auto& model=data.models.at(index);if(model.voxels.empty())return {{"model",index},{"voxels",0}};
 std::vector<double> gridMs,extractMs,pureMs,fillMs,rectMs,pureTotal,fillTotal;
 size_t pureQuads=0,fillQuads=0,rectQuads=0,faces=0;
 // First iteration is a warmup and is excluded from statistics.
 for(int run=-1;run<repeats;++run){auto start=Clock::now();Grid grid(data,model);const double gm=Ms(start);start=Clock::now();auto source=Extract(grid,grid.bounds,true);const double em=Ms(start);
  BakeResult pure,fill,rect;
  if(run%2){pure=Bake(source,grid,grid.bounds,false);fill=Bake(source,grid,grid.bounds,true);}else{fill=Bake(source,grid,grid.bounds,true);pure=Bake(source,grid,grid.bounds,false);}
  rect=Bake(source,grid,grid.bounds,false,VoxSurfacePartitionMethod::RectangleDecomposition);
  if(fill.quads>pure.quads)throw std::runtime_error("backfill increased quad count");
  if(run>=0){gridMs.push_back(gm);extractMs.push_back(em);pureMs.push_back(pure.ms);fillMs.push_back(fill.ms);rectMs.push_back(rect.ms);pureTotal.push_back(gm+em+pure.ms);fillTotal.push_back(gm+em+fill.ms);}
  pureQuads=pure.quads;fillQuads=fill.quads;rectQuads=rect.quads;faces=source.quads.size();
 }
 Grid grid(data,model);const auto original=Extract(grid,grid.bounds,true);const auto originalKeys=FaceKeys(original,grid.bounds.lo);
 nlohmann::json chunks=nlohmann::json::object();
 for(int edge:{16,32,64}){
  auto keys=AllChunks(grid,edge);std::set<FaceKey> combined;size_t pq=0,fq=0;double pm=0,fm=0,em=0;
  for(auto key:keys){auto box=ChunkBox(grid,key,edge);auto start=Clock::now();auto source=Extract(grid,box,false);em+=Ms(start);
   auto a=Bake(source,grid,box,false),b=Bake(source,grid,box,true);pq+=a.quads;fq+=b.quads;pm+=a.ms;fm+=b.ms;
   auto sourceKeys=FaceKeys(source,box.lo);for(auto face:sourceKeys)if(!combined.insert(face).second)throw std::runtime_error("chunk seam duplicated a face");
  }
  if(combined!=originalKeys)throw std::runtime_error("chunk halo source differs from whole model");
  chunks[std::to_string(edge)]={{"initial_chunks",keys.size()},{"initial_greedy_quads",pq},{"initial_backfill_quads",fq},
   {"initial_extraction_ms",em},{"initial_greedy_bake_ms",pm},{"initial_backfill_bake_ms",fm},{"halo_coverage","pass"}};
 }
 std::vector<double> fullExtract,fullPure,fullFill,fullPureTotal,fullFillTotal,removedCounts;
 std::map<int,std::vector<double>> chunkExtract,chunkPure,chunkFill,chunkPureTotal,chunkFillTotal,dirtyCounts;
 nlohmann::json events=nlohmann::json::array();
 for(int event=0;event<edits&&!original.quads.empty();++event){const auto q=original.quads[(size_t(event)*original.quads.size()/edits+original.quads.size()/edits/2)%original.quads.size()];
  Cell center{int(q.geometry&255)+grid.bounds.lo[0],int((q.geometry>>16)&255)+grid.bounds.lo[1],int((q.geometry>>8)&255)+grid.bounds.lo[2]};
  const auto removed=Damage(grid,center,3);if(removed.empty())throw std::runtime_error("damage removed no cells");removedCounts.push_back(double(removed.size()));
  auto start=Clock::now();auto source=Extract(grid,grid.bounds,true);double extraction=Ms(start);BakeResult pure,fill;
  if(event%2){pure=Bake(source,grid,grid.bounds,false);fill=Bake(source,grid,grid.bounds,true);}else{fill=Bake(source,grid,grid.bounds,true);pure=Bake(source,grid,grid.bounds,false);}
  fullExtract.push_back(extraction);fullPure.push_back(pure.ms);fullFill.push_back(fill.ms);fullPureTotal.push_back(extraction+pure.ms);fullFillTotal.push_back(extraction+fill.ms);
  nlohmann::json ev={{"center",center},{"removed",removed.size()},{"whole_greedy_ms",extraction+pure.ms},{"whole_backfill_ms",extraction+fill.ms},{"chunks",nlohmann::json::object()}};
  for(int edge:{16,32,64}){start=Clock::now();const auto dirty=DirtyChunks(grid,removed,edge);double bookkeeping=Ms(start),ex=0,pm=0,fm=0;
   size_t pq=0,fq=0;
   for(auto key:dirty){auto box=ChunkBox(grid,key,edge);start=Clock::now();auto chunk=Extract(grid,box,false);ex+=Ms(start);BakeResult a,b;
    if(event%2){a=Bake(chunk,grid,box,false);b=Bake(chunk,grid,box,true);}else{b=Bake(chunk,grid,box,true);a=Bake(chunk,grid,box,false);}
    pm+=a.ms;fm+=b.ms;pq+=a.quads;fq+=b.quads;
   }
   dirtyCounts[edge].push_back(double(dirty.size()));chunkExtract[edge].push_back(ex);chunkPure[edge].push_back(pm);chunkFill[edge].push_back(fm);
   chunkPureTotal[edge].push_back(bookkeeping+ex+pm);chunkFillTotal[edge].push_back(bookkeeping+ex+fm);
   ev["chunks"][std::to_string(edge)]={{"dirty",dirty.size()},{"greedy_ms",bookkeeping+ex+pm},{"backfill_ms",bookkeeping+ex+fm},{"greedy_quads",pq},{"backfill_quads",fq}};
  }
  for(auto v:removed)grid.Set(v.p,v.id);events.push_back(std::move(ev));
  if((event+1)%8==0)std::cout<<" model "<<index<<" destruction "<<event+1<<"/"<<edits<<std::endl;
 }
 for(int edge:{16,32,64}){auto& c=chunks[std::to_string(edge)];c["dirty_count"]=Counts(dirtyCounts[edge]);c["extraction"]=Distribution(chunkExtract[edge]);c["greedy_bake"]=Distribution(chunkPure[edge]);c["backfill_bake"]=Distribution(chunkFill[edge]);c["greedy_total"]=Distribution(chunkPureTotal[edge]);c["backfill_total"]=Distribution(chunkFillTotal[edge]);}
 return {{"model",index},{"voxels",model.voxels.size()},{"occupied_dimensions",Cell{grid.bounds.hi[0]-grid.bounds.lo[0],grid.bounds.hi[1]-grid.bounds.lo[1],grid.bounds.hi[2]-grid.bounds.lo[2]}},
  {"exposed_faces",faces},{"greedy_quads",pureQuads},{"backfill_quads",fillQuads},{"rectangle_quads",rectQuads},
  {"initial",{{"grid",Distribution(gridMs)},{"extraction",Distribution(extractMs)},{"greedy_bake",Distribution(pureMs)},{"backfill_bake",Distribution(fillMs)},{"rectangle_bake",Distribution(rectMs)},{"greedy_total",Distribution(pureTotal)},{"backfill_total",Distribution(fillTotal)}}},
  {"destruction",{{"radius",3},{"removed_voxels",Counts(removedCounts)},{"whole_extraction",Distribution(fullExtract)},{"whole_greedy_bake",Distribution(fullPure)},{"whole_backfill_bake",Distribution(fullFill)},{"whole_greedy_total",Distribution(fullPureTotal)},{"whole_backfill_total",Distribution(fullFillTotal)},{"chunks",chunks},{"events",events}}}};
}
}
int main(int argc,char** argv)try{
 using namespace VoxDestructionBench;Core::SetLogFileEnabled(false);
 if(argc<3)throw std::runtime_error("usage: MikanVoxDestructionBench output-directory absolute.vox [more.vox ...]");
 std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);
 nlohmann::json report={{"cpu_only",true},{"cache_used",false},{"initial_repeats",5},{"damage_events_per_model",24},{"damage_radius",3},{"gpu_upload_blas_collision_included",false},{"assets",nlohmann::json::array()}};
 for(int argument=2;argument<argc;++argument){const auto path=std::filesystem::absolute(argv[argument]);std::cout<<"Benchmark "<<path.string()<<std::endl;const auto start=Clock::now();VoxFormat::VoxData data;
  if(!VoxFormat::LoadVoxFile(path.string(),data))throw std::runtime_error("VOX load failed: "+path.string());const double load=Ms(start);
  std::map<uint32_t,uint64_t> placements;for(auto i:data.instances)++placements[i.modelIndex];if(placements.empty()&&!data.models.empty())placements[0]=1;
  nlohmann::json asset={{"source",path.string()},{"load_ms",load},{"placed_models",placements.size()},{"models",nlohmann::json::array()}};
  for(auto [index,count]:placements){auto model=BenchmarkModel(data,index,5,24);model["instances"]=count;asset["models"].push_back(std::move(model));}
  std::ofstream(dir/(std::to_string(argument-2)+"-"+path.stem().string()+".json"))<<asset.dump(2)<<'\n';report["assets"].push_back(std::move(asset));std::ofstream(dir/"report.json")<<report.dump(2)<<'\n';
 }
 std::cout<<"All CPU mesh and halo checks passed"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
