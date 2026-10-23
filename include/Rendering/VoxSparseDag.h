#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mikan::vox {
// Uniform leaves carry exact R8 palette indices, including zero/empty.
// Internal nodes are bottom-up, eight children in x/y/z bit order.
inline std::vector<uint32_t> EncodeSparseDag(std::span<const uint8_t> cells,uint32_t sx,uint32_t sy,uint32_t sz){
    if(!sx||!sy||!sz||sx>256||sy>256||sz>256||cells.size()!=size_t(sx)*sy*sz)throw std::runtime_error("Invalid DAG grid");
    uint32_t edge=1;while(edge<sx||edge<sy||edge<sz)edge*=2;
    std::vector<uint32_t> nodes;std::unordered_map<std::string,uint32_t> intern;
    auto build=[&](auto&& self,uint32_t x,uint32_t y,uint32_t z,uint32_t size)->uint32_t{
        if(x>=sx||y>=sy||z>=sz)return 0x80000000u;
        if(size==1)return 0x80000000u|cells[(size_t(z)*sy+y)*sx+x];
        std::array<uint32_t,8> children{};const uint32_t half=size/2;
        for(uint32_t i=0;i<8;++i)children[i]=self(self,x+(i&1)*half,y+((i>>1)&1)*half,z+((i>>2)&1)*half,half);
        bool uniform=(children[0]&0x80000000u)!=0;
        for(auto c:children)uniform&=c==children[0];if(uniform)return children[0];
        std::string key(sizeof(size)+sizeof(children),'\0');std::memcpy(key.data(),&size,sizeof(size));std::memcpy(key.data()+sizeof(size),children.data(),sizeof(children));
        auto [it,inserted]=intern.try_emplace(std::move(key),uint32_t(nodes.size()/8));
        if(inserted)nodes.insert(nodes.end(),children.begin(),children.end());return it->second;
    };
    const uint32_t root=build(build,0,0,0,edge);
    std::vector<uint32_t> result{0x47414456u,1u,sx,sy,sz,root,uint32_t(nodes.size()/8)};
    result.insert(result.end(),nodes.begin(),nodes.end());return result;
}
inline std::vector<uint8_t> DecodeSparseDag(std::span<const uint32_t> words,uint32_t sx,uint32_t sy,uint32_t sz){
    if(words.size()<7||words[0]!=0x47414456u||words[1]!=1||words[2]!=sx||words[3]!=sy||words[4]!=sz||!sx||!sy||!sz||sx>256||sy>256||sz>256||words.size()!=7+size_t(words[6])*8)throw std::runtime_error("Invalid voxel DAG header");
    const uint32_t count=words[6];
    auto valid=[&](uint32_t ref,uint32_t upper){return (ref&0x80000000u)?(ref&0x7fffff00u)==0:ref<upper;};
    if(!valid(words[5],count))throw std::runtime_error("Invalid voxel DAG root");
    for(uint32_t n=0;n<count;++n)for(uint32_t c=0;c<8;++c)if(!valid(words[7+size_t(n)*8+c],n))throw std::runtime_error("Cyclic/invalid voxel DAG");
    uint32_t edge=1;while(edge<sx||edge<sy||edge<sz)edge*=2;
    std::vector<uint8_t> cells(size_t(sx)*sy*sz,0);
    auto fill=[&](auto&& self,uint32_t ref,uint32_t x,uint32_t y,uint32_t z,uint32_t size)->void{
        if(x>=sx||y>=sy||z>=sz)return;
        if(ref&0x80000000u){
            const uint8_t value=uint8_t(ref);if(!value)return;
            for(uint32_t c=z;c<z+size&&c<sz;++c)for(uint32_t b=y;b<y+size&&b<sy;++b)for(uint32_t a=x;a<x+size&&a<sx;++a)cells[(size_t(c)*sy+b)*sx+a]=value;
        }else{
            if(size<=1)throw std::runtime_error("Voxel DAG exceeds grid depth");const uint32_t half=size/2;
            for(uint32_t i=0;i<8;++i)self(self,words[7+size_t(ref)*8+i],x+(i&1)*half,y+((i>>1)&1)*half,z+((i>>2)&1)*half,half);
        }
    };fill(fill,words[5],0,0,0,edge);return cells;
}
}
