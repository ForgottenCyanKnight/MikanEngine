#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace mikan::rt {
constexpr uint32_t kVoxBrickSize=8, kVoxBrickWords=274;
constexpr uint32_t VoxBrickHeaderWords(uint32_t edge){return edge==8?18u:4u;}
constexpr uint32_t VoxBrickWordCount(uint32_t edge){return VoxBrickHeaderWords(edge)+edge*edge*edge/2;}
inline uint32_t VoxBrickEdge(){
    const char* value=std::getenv("MIKAN_VOX_BRICK_SIZE");
    return value&&std::strcmp(value,"2")==0?2u:(value&&std::strcmp(value,"4")==0?4u:8u);
}
struct VoxBrickBox { std::array<uint32_t,3> minimum,maximum; };
struct VoxBrickData { std::vector<VoxBrickBox> boxes; std::vector<uint32_t> words; uint32_t edge=8,wordsPerBrick=274; };
inline uint32_t VoxPaddedSize(uint32_t n){uint32_t p=1;while(p<n)p*=2;return p;}
// Two header words, eight 64-bit occupancy masks (as uint pairs), and
// Packed R8 palette indices + six exposed-face bits per voxel (16 bits).
// Only surface voxels contribute occupancy; bounds are maximum-exclusive.
inline VoxBrickData BuildVoxBricks(const std::vector<uint8_t>& cells,uint32_t sx,uint32_t sy,uint32_t sz,uint32_t edge=8){
    if(edge!=2&&edge!=4&&edge!=8)throw std::runtime_error("Invalid VOX brick size");
    if(!sx||!sy||!sz||sx>256||sy>256||sz>256)throw std::runtime_error("Invalid VOX brick dimensions");
    const uint32_t px=VoxPaddedSize(sx),py=VoxPaddedSize(sy),pz=VoxPaddedSize(sz);
    if(cells.size()!=size_t(px)*py*pz)throw std::runtime_error("Invalid padded VOX grid");
    VoxBrickData result;result.edge=edge;result.wordsPerBrick=VoxBrickWordCount(edge);
    const uint32_t shift=edge==8?3u:(edge==4?2u:1u),header=VoxBrickHeaderWords(edge);
    for(uint32_t bz=0;bz<sz;bz+=edge)for(uint32_t by=0;by<sy;by+=edge)for(uint32_t bx=0;bx<sx;bx+=edge){
        std::array<uint32_t,kVoxBrickWords> w{};uint32_t coarse=0;
        std::array<uint32_t,3> lo{edge,edge,edge},hi{0,0,0};
        for(uint32_t z=0;z<edge&&bz+z<sz;++z)for(uint32_t y=0;y<edge&&by+y<sy;++y)for(uint32_t x=0;x<edge&&bx+x<sx;++x){
            const uint32_t value=cells[(size_t(bz+z)*py+by+y)*px+bx+x];if(!value)continue;
            const std::array<int,3> cell{int(bx+x),int(by+y),int(bz+z)};
            uint32_t faces=0;
            for(uint32_t axis=0;axis<3;++axis)for(int sign: {-1,1}){
                auto neighbor=cell;neighbor[axis]+=sign;
                if(neighbor[0]<0||neighbor[1]<0||neighbor[2]<0||neighbor[0]>=int(sx)||neighbor[1]>=int(sy)||neighbor[2]>=int(sz)||
                   !cells[(size_t(neighbor[2])*py+neighbor[1])*px+neighbor[0]])faces|=1u<<(axis*2+uint32_t(sign>0));
            }
            if(!faces)continue;
            const uint32_t c=(x>>2)|((y>>2)<<1)|((z>>2)<<2),i=x|(y<<shift)|(z<<(shift*2));
            const uint32_t m=edge==2?i:((x&3)|((y&3)<<2)|((z&3)<<4));
            coarse|=1u<<c;w[2+c*2+(m>>5)]|=1u<<(m&31);w[header+i/2]|=(value|(faces<<8))<<((i&1)*16);
            lo={std::min(lo[0],x),std::min(lo[1],y),std::min(lo[2],z)};
            hi={std::max(hi[0],x+1),std::max(hi[1],y+1),std::max(hi[2],z+1)};
        }
        if(!coarse)continue;
        w[0]=bx|(by<<8)|(bz<<16)|(coarse<<24);
        w[1]=lo[0]|(lo[1]<<3)|(lo[2]<<6)|((hi[0]-1)<<9)|((hi[1]-1)<<12)|((hi[2]-1)<<15);
        result.boxes.push_back({{bx+lo[0],by+lo[1],bz+lo[2]},{bx+hi[0],by+hi[1],bz+hi[2]}});
        result.words.insert(result.words.end(),w.begin(),w.begin()+result.wordsPerBrick);
    }
    return result;
}
}
