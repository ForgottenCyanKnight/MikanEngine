#include "Rendering/RayTracing/VoxNaadf.h"
#include <cmath>
#include <iostream>
#include <random>
#include <array>
using I=std::array<int,3>;using V=std::array<double,3>;
struct Volume{I size;std::vector<uint8_t> cells;std::vector<uint32_t> data;
 size_t index(I p)const{return (size_t(p[2])*size[1]+p[1])*size[0]+p[0];}
 uint32_t at(I p)const{for(int a=0;a<3;++a)if(p[a]<0||p[a]>=size[a])return 0;return cells[index(p)];}
 uint32_t lookup(I p,I& lo,I& hi)const{
  const auto& w=data;I c={p[0]/16,p[1]/16,p[2]/16};uint32_t n=w[w[4]+(c[2]*w[2]+c[1])*w[1]+c[0]];int edge=16,bits=5;
  if(n>>30==2){I b={p[0]/4%4,p[1]/4%4,p[2]/4%4};n=w[w[5]+(n&0x3fffffff)+(b[2]*4+b[1])*4+b[0]];edge=4;bits=2;for(int a=0;a<3;++a)c[a]=p[a]/4;
   if(n>>30==2){uint32_t v=(n&0x3fffffff)+(p[2]%4*4+p[1]%4)*4+p[0]%4;n=w[w[6]+v/2]>>(v%2*16)&65535;if(n&32768)return n&32767;edge=1;c=p;}
   else if(n>>30==1)return n&0x3fffffff;
  }else if(n>>30==1)return n&0x3fffffff;
  for(int a=0;a<3;++a){lo[a]=(c[a]-int(n>>(a*2*bits)&((1<<bits)-1)))*edge;hi[a]=(c[a]+int(n>>((a*2+1)*bits)&((1<<bits)-1))+1)*edge;}return 0;
 }
};
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
struct Hit{int material=0;double t=0;int axis=0;};
Hit trace(const Volume& v,V o,V d,bool fast){
 double enter=-1e30,leave=1e30;int axis=0;I step;
 for(int a=0;a<3;++a){step[a]=(d[a]>0)-(d[a]<0);if(!step[a]){if(o[a]<0||o[a]>=v.size[a])return {};continue;}
  double x=-o[a]/d[a],y=(v.size[a]-o[a])/d[a];if(std::min(x,y)>enter){enter=std::min(x,y);axis=a;}leave=std::min(leave,std::max(x,y));}
 double t=std::max(enter,0.001);if(t>leave)return {};I p;
 for(int a=0;a<3;++a)p[a]=std::clamp(int(std::floor(o[a]+t*d[a]+step[a]*1e-4)),0,v.size[a]-1);
 for(int it=0;it<10000&&t<=leave;++it){for(int a=0;a<3;++a)if(p[a]<0||p[a]>=v.size[a])return {};
  I lo=p,hi={p[0]+1,p[1]+1,p[2]+1};uint32_t material=fast?v.lookup(p,lo,hi):v.at(p);if(material)return {int(material),t,axis};
  V boundary;for(int a=0;a<3;++a)boundary[a]=step[a]?((step[a]>0?hi[a]:lo[a])-o[a])/d[a]:1e30;
  double next=std::min({boundary[0],boundary[1],boundary[2]});axis=boundary[0]<=boundary[1]&&boundary[0]<=boundary[2]?0:boundary[1]<=boundary[2]?1:2;
  require(next>t-1e-9,"nonmonotonic traversal");t=next;
  for(int a=0;a<3;++a){p[a]=std::clamp(int(std::floor(o[a]+t*d[a])),lo[a],hi[a]-1);if(boundary[a]<=next)p[a]=step[a]>0?hi[a]:lo[a]-1;}
 }return {};
}
int main(){try{
 std::mt19937 rng(1729);size_t checked=0,rays=0;
 for(I size: {I{1,1,1},I{4,4,4},I{17,19,23},I{64,8,17},I{32,32,32}})for(int pattern=0;pattern<5;++pattern){
  Volume v{size,std::vector<uint8_t>(size_t(size[0])*size[1]*size[2]),{}};
  for(int z=0;z<size[2];++z)for(int y=0;y<size[1];++y)for(int x=0;x<size[0];++x){I p={x,y,z};
   v.cells[v.index(p)]=uint8_t(pattern==0?0:pattern==1?7:pattern==2?(rng()%7==0?rng()%255+1:0):pattern==3?((x==y||z==size[2]/2)?3:0):((x/4+y/4+z/4)%2?0:9));}
  v.data=mikan::render::dda::BuildNaadf(v.cells,size[0],size[1],size[2]);
  for(int z=0;z<size[2];++z)for(int y=0;y<size[1];++y)for(int x=0;x<size[0];++x){I p={x,y,z},lo,hi;auto m=v.lookup(p,lo,hi);require(m==v.at(p),"material mismatch");
   if(!m){for(int a=0;a<3;++a)require(lo[a]<=p[a]&&hi[a]>p[a],"box excludes origin");
    for(int iz=lo[2];iz<hi[2];++iz)for(int iy=lo[1];iy<hi[1];++iy)for(int ix=lo[0];ix<hi[0];++ix)require(v.at({ix,iy,iz})==0,"nonempty cuboid");}++checked;}
  for(int i=0;i<4000;++i){V o,d;for(int a=0;a<3;++a){o[a]=(double(rng())/rng.max()*3-1)*size[a];d[a]=double(rng())/rng.max()*2-1;}if(i%7==0)d[i%3]=0;
   auto a=trace(v,o,d,false),b=trace(v,o,d,true);require(a.material==b.material,"ray material mismatch");if(a.material)require(std::abs(a.t-b.t)<1e-7&&a.axis==b.axis,"ray hit mismatch");++rays;}
 }
 bool rejected=false;try{mikan::render::dda::BuildNaadf({},1,1,1);}catch(const std::invalid_argument&){rejected=true;}require(rejected,"invalid volume accepted");
 std::cout<<"PASS: "<<checked<<" cells checked (material and empty cuboid), "<<rays<<" reference ray comparisons\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
