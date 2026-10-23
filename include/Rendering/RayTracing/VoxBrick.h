#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <string>

namespace mikan::rt {
constexpr uint32_t kVoxBrickSize=8, kVoxBrickWords=274;
constexpr uint32_t VoxBrickHeaderWords(uint32_t edge){return edge==8?18u:4u;}
constexpr uint32_t VoxBrickWordCount(uint32_t edge){return VoxBrickHeaderWords(edge)+edge*edge*edge/2;}
inline uint32_t VoxBrickEdge(){
    const char* value=std::getenv("MIKAN_VOX_BRICK_SIZE");
    // 4^3 balances secondary-ray DDA work against procedural BLAS size.
    // Keep 8^3 and 2^3 available for scene/device-specific comparisons.
    return value&&std::strcmp(value,"2")==0?2u:(value&&std::strcmp(value,"8")==0?8u:4u);
}
struct VoxBrickBox { std::array<uint32_t,3> minimum,maximum; };
struct VoxBrickData { std::vector<VoxBrickBox> boxes; std::vector<uint32_t> words; uint32_t edge=8,wordsPerBrick=274; };
inline uint32_t VoxPaddedSize(uint32_t n){uint32_t p=1;while(p<n)p*=2;return p;}
// Two header words, eight 64-bit occupancy masks (as uint pairs), and
// Packed R8 palette indices + six exposed-face bits per voxel (16 bits).
// Only surface voxels contribute occupancy; bounds are maximum-exclusive.
inline VoxBrickData BuildVoxBricks(const std::vector<uint8_t>& cells,uint32_t sx,uint32_t sy,uint32_t sz,uint32_t edge=8,bool deduplicate=false){
    if(edge!=2&&edge!=4&&edge!=8)throw std::runtime_error("Invalid VOX brick size");
    if(!sx||!sy||!sz||sx>256||sy>256||sz>256)throw std::runtime_error("Invalid VOX brick dimensions");
    const uint32_t px=VoxPaddedSize(sx),py=VoxPaddedSize(sy),pz=VoxPaddedSize(sz);
    if(cells.size()!=size_t(px)*py*pz)throw std::runtime_error("Invalid padded VOX grid");
    VoxBrickData result;result.edge=edge;result.wordsPerBrick=VoxBrickWordCount(edge);
    const uint32_t shift=edge==8?3u:(edge==4?2u:1u),header=VoxBrickHeaderWords(edge);
    const char* skipOption=std::getenv("MIKAN_VOX_BRICK_LOCAL_SKIP");
    const bool localSkip=skipOption&&skipOption[0]=='1'&&(edge==4||edge==8);
    const uint32_t extra=localSkip?(edge==4?32u:512u):0u;
    result.wordsPerBrick+=extra;
    for(uint32_t bz=0;bz<sz;bz+=edge)for(uint32_t by=0;by<sy;by+=edge)for(uint32_t bx=0;bx<sx;bx+=edge){
        std::vector<uint32_t> w(result.wordsPerBrick,0);uint32_t coarse=0;
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
        if(localSkip){
            w[1]|=0x80000000u;
            const uint32_t volume=edge*edge*edge,meta=header+volume/2,bits=edge==4?2u:3u;
            std::vector<uint8_t> used(volume,0);
            auto index=[&](uint32_t x,uint32_t y,uint32_t z){return x|(y<<shift)|(z<<(shift*2));};
            auto empty=[&](uint32_t x,uint32_t y,uint32_t z){uint32_t i=index(x,y,z);return !used[i]&&((w[header+i/2]>>((i&1)*16))&65535u)==0;};
            for(uint32_t z=0;z<edge;++z)for(uint32_t y=0;y<edge;++y)for(uint32_t x=0;x<edge;++x){
                if(!empty(x,y,z))continue;
                uint32_t hx=x+1,hy=y+1,hz=z+1;
                while(hx<edge&&empty(hx,y,z))++hx;
                while(hy<edge){bool ok=true;for(uint32_t a=x;a<hx;++a)ok&=empty(a,hy,z);if(!ok)break;++hy;}
                while(hz<edge){bool ok=true;for(uint32_t b=y;b<hy;++b)for(uint32_t a=x;a<hx;++a)ok&=empty(a,b,hz);if(!ok)break;++hz;}
                uint32_t code=x|(y<<bits)|(z<<(bits*2))|((hx-1)<<(bits*3))|((hy-1)<<(bits*4))|((hz-1)<<(bits*5));
                for(uint32_t c=z;c<hz;++c)for(uint32_t b=y;b<hy;++b)for(uint32_t a=x;a<hx;++a){uint32_t i=index(a,b,c);used[i]=1;if(edge==4)w[meta+i/2]|=code<<((i&1)*16);else w[meta+i]=code;}
            }
        }
        result.boxes.push_back({{bx+lo[0],by+lo[1],bz+lo[2]},{bx+hi[0],by+hi[1],bz+hi[2]}});
        result.words.insert(result.words.end(),w.begin(),w.begin()+result.wordsPerBrick);
    }
    const char* dedupOption=std::getenv("MIKAN_VOX_BRICK_DEDUP");
    if(deduplicate&&(!dedupOption||dedupOption[0]!='0')){
        // Share exact local surface/palette/skip payloads; positions remain per primitive.
        std::unordered_map<std::string,uint32_t> dictionary;
        std::vector<uint32_t> unique,records;
        for(size_t i=0;i<result.boxes.size();++i){
            const auto first=result.words.begin()+i*result.wordsPerBrick;
            std::vector<uint32_t> local(first,first+result.wordsPerBrick);
            const uint32_t origin=local[0]&0xffffffu;local[0]&=0xff000000u;
            std::string key(reinterpret_cast<const char*>(local.data()),local.size()*4);
            auto [it,inserted]=dictionary.try_emplace(std::move(key),uint32_t(unique.size()));
            if(inserted)unique.insert(unique.end(),local.begin(),local.end());
            records.push_back(origin);records.push_back(it->second);
        }
        // Keep the direct layout if a dictionary would increase memory.
        if(4+records.size()+unique.size()<result.words.size()){
            std::vector<uint32_t> packed{0x42524401u,result.wordsPerBrick,uint32_t(result.boxes.size()),uint32_t(4+records.size())};
            packed.insert(packed.end(),records.begin(),records.end());packed.insert(packed.end(),unique.begin(),unique.end());result.words=std::move(packed);
        }
    }
    return result;
}
}
