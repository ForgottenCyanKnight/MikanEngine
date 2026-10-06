#include "../src/Rendering/RayTracing/EmissiveLightTree.h"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace mikan::rt::lights;
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Interval {uint32_t first,count;};
static std::vector<Interval> Intervals(const Tree& tree,glm::vec3 point){
    std::vector<Interval> result(tree.leafNodes.size());
    std::function<void(uint32_t,uint32_t,uint32_t)> visit=[&](uint32_t at,uint32_t first,uint32_t count){
        const auto& n=tree.nodes[at];if(n.child&LeafBit){result[n.child&~LeafBit]={first,count};return;}
        uint32_t left=LeftCount(tree.nodes[n.child],tree.nodes[n.child+1],point,count);
        visit(n.child,first,left);visit(n.child+1,first+left,count-left);
    };visit(0,0,SelectorCount);return result;
}
static uint32_t Pick(const Tree& tree,glm::vec3 point,uint32_t target,uint32_t& count){
    uint32_t at=0,first=0;count=SelectorCount;
    while(!(tree.nodes[at].child&LeafBit)){const auto& n=tree.nodes[at];uint32_t left=LeftCount(tree.nodes[n.child],tree.nodes[n.child+1],point,count);
        if(target<first+left){at=n.child;count=left;}else{at=n.child+1;first+=left;count-=left;}}
    return tree.nodes[at].child&~LeafBit;
}
static uint32_t Replay(const Tree& tree,glm::vec3 point,uint32_t emitter){
    uint32_t at=tree.leafNodes[emitter],trail=0,depth=0;
    while(at){const auto& n=tree.nodes[at];const auto& p=tree.nodes[n.parent];trail=(trail<<1)|uint32_t(at==p.child+1);at=n.parent;++depth;}
    Check(depth<=24,"depth capacity");uint32_t count=SelectorCount;at=0;
    for(uint32_t i=0;i<depth;++i){const auto& n=tree.nodes[at];uint32_t left=LeftCount(tree.nodes[n.child],tree.nodes[n.child+1],point,count);
        bool right=(trail>>i)&1;count=right?count-left:left;at=n.child+uint32_t(right);}
    Check((tree.nodes[at].child&~LeafBit)==emitter,"replay identity");return count;
}
static std::vector<glm::vec4> Fixture(uint32_t count){
    std::vector<glm::vec4> data(1,glm::vec4(float(count),0,0,0));
    for(uint32_t i=0;i<count;++i){float x=float(i%31)*3,y=float(i/31)*2;
        data.emplace_back(glm::vec3(i%7==0?1e-12f:(i%11==0?1e12f:float(1+i%5))),.5f);
        data.emplace_back(x,y,0,0);data.emplace_back(x+1,y,0,0);data.emplace_back(x,y+1,0,0);}
    return data;
}
int main(){try{
    uint64_t checked=0;
    for(uint32_t count:{1u,2u,7u,257u,4096u}){
        auto data=Fixture(count);Tree tree;Check(Build(data,count,tree),"build");Check(tree.nodes.size()==2ull*count-1,"node count");
        for(glm::vec3 point:{glm::vec3(0),glm::vec3(.25f,.25f,0),glm::vec3(3,6,2),glm::vec3(-1e6f,1e6f,10)}){
            auto intervals=Intervals(tree,point);uint64_t sum=0;
            for(uint32_t i=0;i<count;++i){const auto v=intervals[i];Check(v.count>0,"lost dim-light support");sum+=v.count;
                Check(Replay(tree,point,i)==v.count,"forward/reverse probability");
                for(uint32_t target:{v.first,v.first+v.count-1}){uint32_t represented;Check(Pick(tree,point,target,represented)==i,"endpoint selection");Check(represented==v.count,"selected PMF");++checked;}}
            Check(sum==SelectorCount,"normalization");
        }
        std::vector<HitKey> keys;for(uint32_t i=0;i<count;++i)keys.push_back({i%3,i%6,i/3,i});
        Check(Append(data,count,keys),"packed tree");uint32_t keyBase=std::bit_cast<uint32_t>(data[0].w),leafBase=std::bit_cast<uint32_t>(data[0].z);
        Check(keyBase==leafBase+count,"tail offsets");
        for(uint32_t i=0;i<count;++i){const auto& packed=data[keyBase+i];uint32_t emitter=std::bit_cast<uint32_t>(packed.w);
            Check(std::bit_cast<uint32_t>(data[leafBase+emitter].x)==tree.leafNodes[emitter],"packed identity");}
    }
    Tree tree;auto bad=Fixture(2);bad[2].x=INFINITY;Check(!Build(bad,2,tree),"nonfinite geometry accepted");
    auto duplicate=Fixture(2);Check(!Append(duplicate,2,{{0,0,0,0},{0,0,0,1}}),"duplicate identity accepted");
    Check(!Append(duplicate,2,{{0,0,0,0},{0,0,1,0}}),"duplicate emitter accepted");
    auto zero=Fixture(1);zero[1].w=0;Check(!Build(zero,1,tree),"zero area accepted");
    std::vector<glm::vec4> empty(1,glm::vec4(0));Check(Append(empty,0,{}),"empty scene");
    // Independent grid oracle: mirrors the expand shader's integer corner masks,
    // then verifies both primitive halves after affine / mirrored transformation.
    const uint32_t uvMasks[6]={0b10110100u,0b01111000u,0b00101101u,0b01111000u,0b01111000u,0b11010010u};
    for(uint32_t face=0;face<6;++face)for(float sx:{1.f,-2.f}){
        glm::mat4 model(1);model[0][0]=sx;model[1][1]=3;model[2][2]=.5f;model[3]=glm::vec4(13,-8,2,1);
        const uint32_t g=2u|(3u<<8)|(4u<<16)|(6u<<24),a=4u;glm::vec3 bounds(-3,-2,-1);
        auto corners=VoxCorners(g,a,face,.125f,bounds,model);
        for(uint32_t c=0;c<4;++c){uint32_t bits=(uvMasks[face]>>(2*c))&3u;float u=float(bits&1),v=float(bits>>1);glm::vec3 p(2,3,4);
            if(face==0)p.z+=1;if(face==3)p.x+=1;if(face==4)p.y+=1;
            if(face<2){p.x+=u*7;p.y+=v*5;}else if(face<4){p.z+=u*7;p.y+=v*5;}else{p.x+=u*7;p.z+=v*5;}
            auto expected=glm::vec3(model*glm::vec4(p*.125f+bounds,1));Check(corners[c]==expected,"VOX BLAS corner mapping");}
    }
    // Monte Carlo mean checks compensation against a known finite discrete integral.
    auto data=Fixture(7);Check(Build(data,7,tree),"mean build");glm::vec3 point(4,8,2);auto intervals=Intervals(tree,point);
    double truth=0,second=0;for(uint32_t i=0;i<7;++i){double p=double(intervals[i].count)/SelectorCount;double f=double(1+i)/7;truth+=f;second+=f*f/p;}
    std::mt19937 rng(139);double sum=0;constexpr uint32_t samples=500000;
    for(uint32_t s=0;s<samples;++s){uint32_t represented;uint32_t i=Pick(tree,point,rng()&(SelectorCount-1),represented);sum+=(double(1+i)/7)/(double(represented)/SelectorCount);}
    double error=std::abs(sum/samples-truth),sigma=std::sqrt((second-truth*truth)/samples);Check(error<6*sigma+1e-10,"mean energy");
    std::cout<<"PASS: "<<checked<<" interval endpoints; PMF replay, support, packing, invalid input, mean error="<<error<<" (sigma="<<sigma<<")\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
