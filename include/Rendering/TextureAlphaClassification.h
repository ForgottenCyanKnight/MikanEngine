#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// Conservative LOD0 alpha classification. Unknown always executes the exact
// shader test. The 8x8 block ranges include every decoded texel, not samples.
struct TextureAlphaClassification {
    enum State : uint32_t { Unknown=0, Opaque=1, Transparent=2 };
    struct Range { uint8_t lo=255,hi=0; };
    uint32_t width=0,height=0,columns=0;
    uint8_t minimum=255,maximum=0;
    std::vector<Range> blocks;
    State Whole() const {
        if(!width || !height)return Unknown;
        return minimum>=128?Opaque:(maximum<128?Transparent:Unknown);
    }
    void Build(const uint8_t* rgba,size_t size,uint32_t w,uint32_t h){
        *this={};
        if(!rgba || !w || !h || uint64_t(w)*h>std::numeric_limits<size_t>::max()/4 || size!=size_t(w)*h*4)return;
        width=w;height=h;columns=(w+7)/8;
        for(size_t i=3;i<size;i+=4){minimum=std::min(minimum,rgba[i]);maximum=std::max(maximum,rgba[i]);}
        if(Whole()!=Unknown)return;
        blocks.resize(size_t(columns)*((h+7)/8));
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            auto& b=blocks[size_t(y/8)*columns+x/8];const auto a=rgba[(size_t(y)*w+x)*4+3];
            b.lo=std::min(b.lo,a);b.hi=std::max(b.hi,a);
        }
    }
    State Classify(float u0,float v0,float u1,float v1,float u2,float v2,uint32_t padding=1) const {
        const auto whole=Whole();if(whole!=Unknown)return whole;
        if(blocks.empty())return Unknown;
        for(float v:{u0,v0,u1,v1,u2,v2})if(!std::isfinite(v))return Unknown;
        const double lx=std::floor(double(std::min({u0,u1,u2}))*width-.5)-padding;
        const double hx=std::floor(double(std::max({u0,u1,u2}))*width-.5)+padding+1;
        const double ly=std::floor(double(std::min({v0,v1,v2}))*height-.5)-padding;
        const double hy=std::floor(double(std::max({v0,v1,v2}))*height-.5)+padding+1;
        // Include bilinear support plus an extra texel for rounding. Boundary
        // and wrapping cases stay unknown, independently of sampler address mode.
        if(lx<0 || ly<0 || hx>=width || hy>=height)return Unknown;
        uint8_t lo=255,hi=0;
        for(uint32_t y=uint32_t(ly)/8;y<=uint32_t(hy)/8;++y)
            for(uint32_t x=uint32_t(lx)/8;x<=uint32_t(hx)/8;++x){
                const auto& b=blocks[size_t(y)*columns+x];lo=std::min(lo,b.lo);hi=std::max(hi,b.hi);
                if(lo<128 && hi>=128)return Unknown;
            }
        return lo>=128?Opaque:(hi<128?Transparent:Unknown);
    }
};
