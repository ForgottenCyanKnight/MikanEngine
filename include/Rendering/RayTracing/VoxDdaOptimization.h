#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mikan::render::dda {
// Header: padded xyz, mip count, then one relative word offset per mip > 0.
// Each level is independently word aligned; level zero remains the palette texture.
inline std::vector<uint32_t> BuildBitsetMips(const std::vector<uint8_t>& cells,
                                           uint32_t x,uint32_t y,uint32_t z){
    if(!x||!y||!z||cells.size()!=size_t(x)*y*z || (x&(x-1)) || (y&(y-1)) || (z&(z-1)))
        throw std::invalid_argument("DDA occupancy requires a padded power-of-two volume");
    uint32_t levels=1;
    for(uint32_t n=std::max({x,y,z});n>1;n>>=1)++levels;
    std::vector<uint32_t> words(4+levels-1,0);
    words[0]=x;words[1]=y;words[2]=z;words[3]=levels;
    std::vector<uint8_t> current(cells);
    for(uint32_t level=1;level<levels;++level){
        const uint32_t nx=std::max(1u,x/2),ny=std::max(1u,y/2),nz=std::max(1u,z/2);
        std::vector<uint8_t> next(size_t(nx)*ny*nz,0);
        for(uint32_t cz=0;cz<z;++cz)for(uint32_t cy=0;cy<y;++cy)for(uint32_t cx=0;cx<x;++cx)
            if(current[(size_t(cz)*y+cy)*x+cx])next[(size_t(cz/2)*ny+cy/2)*nx+cx/2]=1;
        const size_t base=words.size();words[4+level-1]=uint32_t(base);
        words.resize(base+(next.size()+31)/32,0);
        for(size_t i=0;i<next.size();++i)if(next[i])words[base+i/32]|=1u<<(i&31);
        current=std::move(next);x=nx;y=ny;z=nz;
    }
    return words;
}

// Fail open for a camera-plane crossing or nonfinite transform. The padded
// rectangle is in Vulkan NDC, matching the primary ray's unjittered pixel index.
inline glm::vec4 ProjectBounds(const glm::mat4& clipFromLocal,glm::vec3 minimum,
                               glm::vec3 maximum,uint32_t width,uint32_t height){
    const glm::vec4 full(-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    if(!width||!height)return full;
    glm::vec2 lo(std::numeric_limits<float>::max()),hi(-std::numeric_limits<float>::max());
    for(uint32_t corner=0;corner<8;++corner){
        const glm::vec3 p((corner&1)?maximum.x:minimum.x,(corner&2)?maximum.y:minimum.y,(corner&4)?maximum.z:minimum.z);
        const auto clip=clipFromLocal*glm::vec4(p,1);
        if(!std::isfinite(clip.w)||clip.w<=1e-4f)return full;
        const glm::vec2 ndc=glm::vec2(clip)/clip.w;
        if(!std::isfinite(ndc.x)||!std::isfinite(ndc.y))return full;
        lo=glm::min(lo,ndc);hi=glm::max(hi,ndc);
    }
    const glm::vec2 margin(8.f/float(width),8.f/float(height)); // four pixels, including jitter
    return glm::vec4(lo-margin,hi+margin);
}
}
