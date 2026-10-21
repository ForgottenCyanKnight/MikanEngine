#include "Rendering/RayTracing/VoxBrick.h"
#include <cassert>
#include <iostream>
#include <random>
int main(){
    std::mt19937 random(1729);uint64_t checked=0;
    for(uint32_t edge: {2u,4u,8u})for(auto dims: {std::array<uint32_t,3>{1,1,1},{9,13,5},{16,16,16},{32,8,17}}){
        const auto px=mikan::rt::VoxPaddedSize(dims[0]),py=mikan::rt::VoxPaddedSize(dims[1]),pz=mikan::rt::VoxPaddedSize(dims[2]);
        for(uint32_t density: {0u,5u,100u}){
            std::vector<uint8_t> cells(size_t(px)*py*pz,0);
            std::vector<uint16_t> decoded(cells.size(),0),expected(cells.size(),0);
            for(uint32_t z=0;z<dims[2];++z)for(uint32_t y=0;y<dims[1];++y)for(uint32_t x=0;x<dims[0];++x)
                if(random()%100<density)cells[(size_t(z)*py+y)*px+x]=uint8_t(1+random()%255);
            for(uint32_t z=0;z<dims[2];++z)for(uint32_t y=0;y<dims[1];++y)for(uint32_t x=0;x<dims[0];++x){
                const size_t offset=(size_t(z)*py+y)*px+x;if(!cells[offset])continue;
                uint32_t faces=0;
                for(uint32_t a=0;a<3;++a)for(int sign: {-1,1}){
                    std::array<int,3> n{int(x),int(y),int(z)};n[a]+=sign;
                    const bool outside=n[0]<0||n[1]<0||n[2]<0||n[0]>=int(dims[0])||n[1]>=int(dims[1])||n[2]>=int(dims[2]);
                    if(outside||cells[(size_t(n[2])*py+n[1])*px+n[0]]==0)faces|=1u<<(a*2+uint32_t(sign>0));
                }
                expected[offset]=faces?uint16_t(cells[offset]|(faces<<8)):0;
            }
            const auto data=mikan::rt::BuildVoxBricks(cells,dims[0],dims[1],dims[2],edge);
            assert(data.words.size()==data.boxes.size()*data.wordsPerBrick);
            const uint32_t shift=edge==8?3u:(edge==4?2u:1u),header=mikan::rt::VoxBrickHeaderWords(edge);
            for(size_t b=0;b<data.boxes.size();++b){
                const auto* w=data.words.data()+b*data.wordsPerBrick;const uint32_t ox=w[0]&255,oy=(w[0]>>8)&255,oz=(w[0]>>16)&255;
                uint32_t coarse=0;std::array<uint32_t,3> lo{256,256,256},hi{};
                for(uint32_t z=0;z<edge;++z)for(uint32_t y=0;y<edge;++y)for(uint32_t x=0;x<edge;++x){
                    const uint32_t i=x|(y<<shift)|(z<<(shift*2)),value=(w[header+i/2]>>((i&1)*16))&65535;
                    const uint32_t c=(x>>2)|((y>>2)<<1)|((z>>2)<<2),m=edge==2?i:((x&3)|((y&3)<<2)|((z&3)<<4));
                    assert(bool(value)==bool(w[2+c*2+(m>>5)]&(1u<<(m&31))));
                    if(!value)continue;assert(ox+x<dims[0]&&oy+y<dims[1]&&oz+z<dims[2]);coarse|=1u<<c;
                    auto& target=decoded[(size_t(oz+z)*py+oy+y)*px+ox+x];assert(target==0);target=uint16_t(value);
                    lo={std::min(lo[0],ox+x),std::min(lo[1],oy+y),std::min(lo[2],oz+z)};
                    hi={std::max(hi[0],ox+x+1),std::max(hi[1],oy+y+1),std::max(hi[2],oz+z+1)};++checked;
                }
                assert(coarse==(w[0]>>24));assert(lo==data.boxes[b].minimum&&hi==data.boxes[b].maximum);
            }
            assert(decoded==expected);
        }
    }
    bool rejected=false;try{mikan::rt::BuildVoxBricks({},9,9,9);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    assert(mikan::rt::VoxBrickWordCount(2)==8&&mikan::rt::VoxBrickWordCount(4)==36&&mikan::rt::VoxBrickWordCount(8)==274);
    bool badEdge=false;try{mikan::rt::BuildVoxBricks({1},1,1,1,3);}catch(const std::runtime_error&){badEdge=true;}assert(badEdge);
    std::cout<<"Brick surface faces/bounds/occupancy/palette roundtrip PASS: "<<checked<<" voxels\n";
}
