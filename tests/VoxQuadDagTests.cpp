#include "Rendering/RayTracing/VoxQuadDag.h"
#include <cassert>
#include <iostream>
int main(){using namespace mikan::rt;
 std::vector<DagQuad> q{{0,0,0,2,3,0,0},{1,1,1,1,1,3,1},{9,0,0,2,3,0,2},{10,1,1,1,1,3,3}};
 auto dag=CompileVoxQuadDag(q,9);assert(dag.surfaces.size()==2&&dag.uniqueBlocks==1&&dag.uniqueQuads==2);
 std::vector<bool> covered(4);for(const auto& s:dag.surfaces)for(size_t i=0;i<s.quads.size();++i){auto id=s.sources[i];assert(!covered[id]);covered[id]=true;
  auto it=std::upper_bound(s.planes.begin(),s.planes.end(),uint32_t(i),[](auto n,const auto& p){return n<p.firstQuad;});auto d=DecodeVoxQuad(s.quads[i].geometry,std::prev(it)->planeDirection);
  assert((d.first&255u)+s.origin[0]==q[id].x);assert(((d.first>>8)&255u)+s.origin[1]==q[id].y);assert(((d.first>>16)&255u)+s.origin[2]==q[id].z);
 }
 for(bool v:covered)assert(v);
 q[3].width=2;assert(CompileVoxQuadDag(q,9).uniqueBlocks==2);
 // A quad crossing a partition remains intact, with its full dimensions.
 q={{8,0,0,20,1,0,0}};assert(((CompileVoxQuadDag(q,9).surfaces[0].quads[0].geometry>>16)&255u)==19u);
 std::vector<uint32_t> attrs{4,8,10,0,0xff000000,0,0x00ff0000,4,0x80000000,1,0x01000100};
 std::vector<uint32_t> ids{1,0};std::vector<VoxQuad> quads{{0},{1u<<16|1u<<24}};
 auto a=SliceVoxDagAttributes(attrs,ids,quads);assert(a[a[1]]==1&&a[a[1]+1]==0x80000000&&a[a[2]]==0x01000100);
 // All six orientations, partition boundaries and multiple selected levels.
 std::vector<DagQuad> many;
 for(uint32_t face=0;face<6;++face)for(uint32_t x=0;x<82;x+=8)many.push_back({x,(x*3)%80,(x*7)%80,1+x%17,1+x%11,face,uint32_t(many.size())});
 for(auto edge:{3u,9u,27u,81u}){auto graph=CompileVoxQuadDag(many,edge);std::vector<bool> seen(many.size());
  for(const auto& surface:graph.surfaces)for(uint32_t i=0;i<surface.quads.size();++i){const auto id=surface.sources[i];assert(!seen[id]);seen[id]=true;
   auto it=std::upper_bound(surface.planes.begin(),surface.planes.end(),i,[](auto n,const auto& p){return n<p.firstQuad;});
   auto d=DecodeVoxQuad(surface.quads[i].geometry,std::prev(it)->planeDirection);const auto& q=many[id];
   assert((d.first&255u)+surface.origin[0]==q.x&&((d.first>>8)&255u)+surface.origin[1]==q.y&&((d.first>>16)&255u)+surface.origin[2]==q.z);
   assert((d.first>>24)+1==q.width&&(d.second&255u)+1==q.height&&surface.directions[i]==q.face);
  }for(bool v:seen)assert(v);
  for(uint32_t n=0;n<graph.nodes.size();++n){const auto& words=graph.nodes[n];if(words[0]==1)for(size_t i=3;i<words.size();i+=4)assert(words[i]<n);}
 }
 // Mixed cut keeps costly rare geometry in one residual; coverage unchanged.
 std::vector<DagQuad> repeated;
 for(uint32_t copy=0;copy<10;++copy)for(uint32_t k=0;k<20;++k)repeated.push_back({copy*9+k%3,k/3,0,1,1,0,uint32_t(repeated.size())});
 repeated.push_back({100,0,0,2,2,3,uint32_t(repeated.size())});
 auto cut=CompileVoxQuadDag(repeated,9);SelectVoxDagCut(cut,repeated,100,10);
 assert(cut.uniqueBlocks==2&&cut.surfaces.size()==11&&cut.uniqueQuads==21);
 std::vector<bool> seenCut(repeated.size());for(const auto& part:cut.surfaces)for(auto id:part.sources){assert(!seenCut[id]);seenCut[id]=true;}for(bool seen:seenCut)assert(seen);
 // Long/straddling quads omitted from shareable nodes still enter residual.
 auto partial=CompileVoxQuadDag(std::span<const DagQuad>(repeated.data(),repeated.size()-1),9);SelectVoxDagCut(partial,repeated,100,10);
 size_t total=0;for(const auto& part:partial.surfaces)total+=part.sources.size();assert(total==repeated.size());
 bool threw=false;try{CompileVoxQuadDag(q,8);}catch(...){threw=true;}assert(threw);
 std::cout<<"Quad DAG topology, coverage, crossing and attributes passed\n";
}
