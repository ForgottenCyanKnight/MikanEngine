#include "Rendering/RayTracing/VoxDdaOptimization.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
using namespace mikan::render::dda;
void Require(bool value){if(!value)throw std::runtime_error("DDA optimization check failed");}
int main(){
    // Non-cubic volumes, singleton axes, empty and full hierarchies, odd raw
    // dimensions padded with zero: compare every bit with a brute-force block.
    for(const auto dims:{glm::uvec3(1),glm::uvec3(8,4,2),glm::uvec3(1,8,4),glm::uvec3(32,16,8)})
    for(uint32_t pattern=0;pattern<3;++pattern){
        std::vector<uint8_t> cells(size_t(dims.x)*dims.y*dims.z);
        for(size_t i=0;i<cells.size();++i)cells[i]=pattern==0?0:pattern==1?255:uint8_t((i*37+11)%7==0);
        const auto words=BuildBitsetMips(cells,dims.x,dims.y,dims.z);
        for(uint32_t level=1;level<words[3];++level){
            auto d=glm::max(dims>>level,glm::uvec3(1));const uint32_t edge=1u<<level;
            for(uint32_t z=0;z<d.z;++z)for(uint32_t y=0;y<d.y;++y)for(uint32_t x=0;x<d.x;++x){
                bool expected=false;
                for(uint32_t cz=z*edge;cz<std::min((z+1)*edge,dims.z);++cz)
                for(uint32_t cy=y*edge;cy<std::min((y+1)*edge,dims.y);++cy)
                for(uint32_t cx=x*edge;cx<std::min((x+1)*edge,dims.x);++cx)
                    expected|=cells[(size_t(cz)*dims.y+cy)*dims.x+cx]!=0;
                const uint32_t i=(z*d.y+y)*d.x+x;
                Require(bool((words[words[4+level-1]+i/32]>>(i&31))&1)==expected);
            }
        }
    }
    bool rejected=false;try{BuildBitsetMips(std::vector<uint8_t>(3),3,1,1);}catch(const std::invalid_argument&){rejected=true;}Require(rejected);
    const auto projection=glm::perspective(glm::radians(45.f),16.f/9.f,.1f,500.f);
    const glm::vec3 lo(-1),hi(1);
    for(float angle:{0.f,.7f,1.4f}){
        const auto transform=projection*glm::translate(glm::mat4(1),glm::vec3(3,1,-10))*glm::rotate(glm::mat4(1),angle,glm::vec3(0,1,0));
        const auto rectangle=ProjectBounds(transform,lo,hi,1280,720);
        for(int z=0;z<=10;++z)for(int y=0;y<=10;++y)for(int x=0;x<=10;++x){
            auto p=transform*glm::vec4(glm::vec3(x,y,z)/5.f-1.f,1);
            auto uv=glm::vec2(p)/p.w;
            Require(uv.x>=rectangle.x&&uv.y>=rectangle.y&&uv.x<=rectangle.z&&uv.y<=rectangle.w);
        }
    }
    auto crossing=ProjectBounds(projection,lo,hi,1280,720);
    Require(crossing.x<-1e20f&&crossing.z>1e20f);
    auto behind=ProjectBounds(projection*glm::translate(glm::mat4(1),glm::vec3(0,0,10)),lo,hi,1280,720);
    Require(behind.x<-1e20f&&behind.z>1e20f);
    std::cout<<"Bitset mip occupancy and conservative projected bounds: PASS\n";
}
