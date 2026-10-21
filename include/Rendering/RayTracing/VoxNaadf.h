#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mikan::render::dda {
// NAADF-inspired, independently implemented: 4^3 voxels/block, 4^3 blocks/chunk.
// Empty nodes store a conservative empty cuboid, NOT six independent ray distances.
// Relative word offsets: magic, chunk xyz, chunks, blocks, packed uint16 voxels, reserved.
// Chunk/block state in bits31:30: empty=0, uniform=1, mixed=2.
// Empty chunk distances use 5 bits/direction; block/voxel distances use 2.
// This experimental builder is synchronous and does not deduplicate blocks.
inline std::vector<uint32_t> BuildNaadf(const std::vector<uint8_t>& cells,
                                      uint32_t sx,uint32_t sy,uint32_t sz){
    if(!sx||!sy||!sz||cells.size()!=size_t(sx)*sy*sz)
        throw std::invalid_argument("NAADF volume size mismatch");
    auto index=[](uint32_t x,uint32_t y,uint32_t z,uint32_t nx,uint32_t ny){return (size_t(z)*ny+y)*nx+x;};
    auto expand=[&](std::vector<uint32_t>& nodes,uint32_t nx,uint32_t ny,uint32_t nz,uint32_t bits,uint32_t stateShift){
        const int dimensions[3]={int(nx),int(ny),int(nz)};
        const int maximum=(1<<bits)-1;
        for(int z=0;z<int(nz);++z)for(int y=0;y<int(ny);++y)for(int x=0;x<int(nx);++x){
            const size_t at=index(x,y,z,nx,ny);if(nodes[at]>>stateShift)continue;
            int lo[3]={x,y,z},hi[3]={x,y,z};const int center[3]={x,y,z};
            bool blocked[6]={};
            for(int iteration=0;iteration<maximum;++iteration)for(int axis=0;axis<3;++axis)for(int sign=0;sign<2;++sign){
                const int direction=axis*2+sign;if(blocked[direction])continue;
                const int slice=sign?hi[axis]+1:lo[axis]-1;
                bool empty=slice>=0&&slice<dimensions[axis];
                if(empty){
                    const int a=(axis+1)%3,b=(axis+2)%3;
                    for(int u=lo[a];u<=hi[a]&&empty;++u)for(int v=lo[b];v<=hi[b];++v){
                        int p[3];p[axis]=slice;p[a]=u;p[b]=v;
                        if(nodes[index(p[0],p[1],p[2],nx,ny)]>>stateShift){empty=false;break;}
                    }
                }
                if(empty){if(sign)hi[axis]=slice;else lo[axis]=slice;}else blocked[direction]=true;
            }
            uint32_t packed=0;for(int a=0;a<3;++a){packed|=uint32_t(center[a]-lo[a])<<(bits*(a*2));packed|=uint32_t(hi[a]-center[a])<<(bits*(a*2+1));}
            nodes[at]=packed;
        }
    };
    const uint32_t cx=(sx+15)/16,cy=(sy+15)/16,cz=(sz+15)/16;
    std::vector<uint32_t> chunks(size_t(cx)*cy*cz),blocks,voxels;
    auto voxel=[&](uint32_t x,uint32_t y,uint32_t z)->uint32_t{return x<sx&&y<sy&&z<sz?cells[index(x,y,z,sx,sy)]:0u;};
    for(uint32_t z=0;z<cz;++z)for(uint32_t y=0;y<cy;++y)for(uint32_t x=0;x<cx;++x){
        std::vector<uint32_t> localBlocks(64);const size_t firstVoxel=voxels.size();
        for(uint32_t bz=0;bz<4;++bz)for(uint32_t by=0;by<4;++by)for(uint32_t bx=0;bx<4;++bx){
            std::vector<uint32_t> localVoxels(64);uint32_t first=0;bool equal=true;
            for(uint32_t vz=0;vz<4;++vz)for(uint32_t vy=0;vy<4;++vy)for(uint32_t vx=0;vx<4;++vx){
                const size_t i=index(vx,vy,vz,4,4);const uint32_t v=voxel(x*16+bx*4+vx,y*16+by*4+vy,z*16+bz*4+vz);
                if(!i)first=v;else equal&=v==first;localVoxels[i]=v?0x8000u|v:0u;
            }
            uint32_t node=first?0x40000000u|first:0u;
            if(!equal){
                expand(localVoxels,4,4,4,2,15);node=0x80000000u|uint32_t(voxels.size());
                voxels.insert(voxels.end(),localVoxels.begin(),localVoxels.end());
            }
            localBlocks[index(bx,by,bz,4,4)]=node;
        }
        const bool uniform=std::all_of(localBlocks.begin(),localBlocks.end(),[&](uint32_t n){return n==localBlocks[0]&&(n>>30)!=2;});
        if(uniform){chunks[index(x,y,z,cx,cy)]=localBlocks[0];voxels.resize(firstVoxel);}
        else{
            expand(localBlocks,4,4,4,2,30);chunks[index(x,y,z,cx,cy)]=0x80000000u|uint32_t(blocks.size());
            blocks.insert(blocks.end(),localBlocks.begin(),localBlocks.end());
        }
    }
    expand(chunks,cx,cy,cz,5,30);
    std::vector<uint32_t> words={0x4e414144u,cx,cy,cz,8u,uint32_t(8+chunks.size()),uint32_t(8+chunks.size()+blocks.size()),0u};
    words.insert(words.end(),chunks.begin(),chunks.end());words.insert(words.end(),blocks.begin(),blocks.end());
    for(size_t i=0;i<voxels.size();i+=2)words.push_back(voxels[i]|(voxels[i+1]<<16));
    return words;
}
}
