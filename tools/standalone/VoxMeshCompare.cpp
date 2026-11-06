// CPU-only A/B comparison. Reads raw VOX; never accesses a voxmesh cache.
#include "Rendering/VoxFaceMaterial.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "Rendering/RayTracing/VoxSurfaceBake.h"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
namespace VoxMeshComparison {
struct Source {
 std::vector<VoxRtQuad> quads; std::vector<uint32_t> materials; VoxelMeshData mesh{};
 const auto& GetQuads()const{return quads;} const auto& GetQuadMaterials()const{return materials;}
 const auto& GetMeshData()const{return mesh;}
};
}
int main(int argc,char** argv)try {
 if(argc!=3)throw std::runtime_error("usage: MikanVoxMeshCompare source.vox report-directory");
 const auto start=std::chrono::steady_clock::now();
 VoxFormat::VoxData data;
 if(!VoxFormat::LoadVoxFile(argv[1],data))throw std::runtime_error("VOX load failed");
 std::filesystem::create_directories(argv[2]); const std::filesystem::path dir=argv[2];
 std::map<uint32_t,uint64_t> placements;
 for(const auto& instance:data.instances)++placements[instance.modelIndex];
 if(placements.empty()&&!data.models.empty())placements[0]=1;
 nlohmann::json rows=nlohmann::json::array();
 uint64_t totals[2][5]{}; double bakeSeconds[2][2]{},sharedSeconds[2]{}; size_t done=0;
 std::ofstream csv(dir/"components.csv");csv<<"model,instances,exposed_faces,old_baseline,new_baseline,old_final,new_final\n";
 for(const auto& [index,count]:placements){
  const auto& model=data.models.at(index);if(model.voxels.empty())continue;
  int minimum[3]{256,256,256},maximum[3]{};
  for(auto v:model.voxels){int p[]{v.x,v.y,v.z};for(int a=0;a<3;++a){minimum[a]=std::min(minimum[a],p[a]);maximum[a]=std::max(maximum[a],p[a]);}}
  int sx=maximum[0]+1,sy=maximum[1]+1,sz=maximum[2]+1;
  std::vector<uint8_t> grid(size_t(sx)*sy*sz);
  auto cell=[&](int x,int y,int z)->uint8_t {return x<0||y<0||z<0||x>=sx||y>=sy||z>=sz?0:grid[(size_t(z)*sy+y)*sx+x];};
  for(auto v:model.voxels)grid[(size_t(v.z)*sy+v.y)*sx+v.x]=v.colorIndex;
  auto ordinary=[&](std::array<int,3> p){auto id=cell(p[0],p[1],p[2]);return id&&(!data.hasMaterials||(data.materials[id].type!=2&&data.materials[id].type!=3));};
  VoxMeshComparison::Source source;
  for(int d=0;d<6;++d){auto& group=source.mesh.faceGroups[d];group.faceDirection=d;group.firstIndex=source.quads.size()*6;
   int axis=d<2?1:(d<4?0:2),sign=(d==0||d==3||d==4)?1:-1;
   for(auto v:model.voxels){int p[]{v.x,v.y,v.z};p[axis]+=sign;if(cell(p[0],p[1],p[2]))continue;
    uint32_t x=v.x-minimum[0],y=v.z-minimum[2],z=v.y-minimum[1];auto c=data.palette[v.colorIndex];
    source.quads.push_back({x|(y<<8)|(z<<16),(uint32_t(c.r)<<8)|(uint32_t(c.g)<<16)|(uint32_t(c.b)<<24)});
    source.materials.push_back(PackVoxFaceMaterial(data,v.colorIndex));
   }group.indexCount=source.quads.size()*6-group.firstIndex;
  }
  VoxContactQuery contact=[&](int d,int p,int u,int v){auto pair=VoxContactRawPair(d,p,u,v,minimum[0],minimum[1],minimum[2]);return ordinary(pair[0])&&ordinary(pair[1]);};
  VoxContactQuery eligible=[&](int d,int p,int u,int v){return ordinary(VoxContactRawPair(d,p,u,v,minimum[0],minimum[1],minimum[2])[0]);};
  uint64_t values[5]{source.quads.size()};
  for(int order=0;order<2;++order){
   const int algorithm=(order+int(done%2))%2;
   auto method=algorithm?VoxSurfacePartitionMethod::RectangleDecomposition:VoxSurfacePartitionMethod::Greedy;
   for(int backfill=0;backfill<2;++backfill){std::vector<VoxRtQuad> q;std::vector<uint32_t> m,a;std::vector<VoxRayTracingRange> r;VoxContactBackfillStats stats;
    const auto bakeStart=std::chrono::steady_clock::now();
    BakeVoxSurfaces(source,true,q,m,r,a,backfill?contact:VoxContactQuery{},&stats,backfill?eligible:VoxContactQuery{},bool(backfill),method);
    bakeSeconds[algorithm][backfill]+=std::chrono::duration<double>(std::chrono::steady_clock::now()-bakeStart).count();
    values[backfill?3+algorithm:1+algorithm]=q.size();
   }
   values[3+algorithm]=std::min(values[3+algorithm],values[1+algorithm]);
   std::vector<VoxRtQuad> q;std::vector<uint32_t> m,a;std::vector<VoxRayTracingRange> r;
   const auto sharedStart=std::chrono::steady_clock::now();
   const auto selection=BakeVoxSurfacesWithBackfill(source,true,q,m,r,a,contact,eligible,method);
   sharedSeconds[algorithm]+=std::chrono::duration<double>(std::chrono::steady_clock::now()-sharedStart).count();
   if(q.size()!=values[3+algorithm]||selection.baselineQuads!=values[1+algorithm])throw std::runtime_error("shared bake count mismatch");
  }
  for(int i=0;i<5;++i){totals[0][i]+=values[i];totals[1][i]+=values[i]*count;}
  csv<<index<<','<<count;for(auto value:values)csv<<','<<value;csv<<'\n';csv.flush();
  rows.push_back({{"model",index},{"instances",count},{"exposed_faces",values[0]},{"old_baseline",values[1]},{"new_baseline",values[2]},{"old_final",values[3]},{"new_final",values[4]}});
  ++done;if(done%8==0||done==placements.size())std::cout<<"Compared "<<done<<"/"<<placements.size()<<" components"<<std::endl;
 }
 auto summary=[&](int algorithm,int weighting){return nlohmann::json{{"exposed_faces",totals[weighting][0]},{"baseline_quads",totals[weighting][1+algorithm]},{"final_quads",totals[weighting][3+algorithm]},{"final_triangles",2*totals[weighting][3+algorithm]}};};
 nlohmann::json report={{"source",argv[1]},{"cache_used",false},{"unique_components",done},{"instances",0},{"merge_colors",true},{"components",rows}};
 for(auto [i,n]:placements)report["instances"]=report["instances"].get<uint64_t>()+n;
 for(int algorithm=0;algorithm<2;++algorithm){auto name=algorithm?"rectangle_decomposition":"old_greedy";nlohmann::json result={{"source",argv[1]},{"cache_used",false},{"baseline_bake_seconds",bakeSeconds[algorithm][0]},{"contact_bake_seconds",bakeSeconds[algorithm][1]},{"two_pass_bake_seconds",bakeSeconds[algorithm][0]+bakeSeconds[algorithm][1]},{"production_bake_seconds",sharedSeconds[algorithm]},{"unique",summary(algorithm,0)},{"instance_weighted",summary(algorithm,1)}};report[name]=result;std::ofstream(dir/(std::string(name)+".json"))<<result.dump(2)<<'\n';}
 report["seconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();std::ofstream(dir/"comparison.json")<<report.dump(2)<<'\n';
 std::cout<<report["old_greedy"].dump(2)<<'\n'<<report["rectangle_decomposition"].dump(2)<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
