#include "../src/Rendering/RayTracing/EmissiveLightIndex.h"
#include <iostream>
#include <stdexcept>
using namespace mikan::rt::lights;
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static std::vector<glm::vec4> Fixture(uint32_t count){
    std::vector<glm::vec4> data(1,glm::vec4(float(count),0,0,0));
    for(uint32_t i=0;i<count;++i){
        data.emplace_back(1,2,3,.5f);
        data.emplace_back(float(i),0,0,float(i+1)/float(count));
        data.emplace_back(float(i)+1,0,0,1.f/float(count));
        data.emplace_back(float(i),1,0,0);
    }
    return data;
}
int main(){try{
    for(uint32_t count:{1u,2u,257u,4096u}){
        auto data=Fixture(count);const auto prefix=data;
        std::vector<HitKey> keys;
        for(uint32_t i=0;i<count;++i)keys.push_back({i%3,i%7,i,count-1-i});
        Check(AppendIndex(data,count,keys),"index build");
        const uint32_t base=std::bit_cast<uint32_t>(data[0].w);
        Check(base==1+4*count&&data.size()==base+count,"index offset/size");
        Check(data[0].x==float(count)&&data[0].y==0&&data[0].z==0,"header/count");
        for(size_t i=1;i<prefix.size();++i)Check(data[i]==prefix[i],"CDF or geometry overwritten");
        for(const auto& key:keys){
            // Independent exhaustive oracle checks every serialized identity and original emitter.
            uint32_t matches=0;
            for(uint32_t j=0;j<count;++j){const auto& v=data[base+j];
                if(std::bit_cast<uint32_t>(v.x)==key.instance&&std::bit_cast<uint32_t>(v.y)==key.geometry&&std::bit_cast<uint32_t>(v.z)==key.primitive){
                    Check(std::bit_cast<uint32_t>(v.w)==key.emitter,"emitter identity changed");++matches;
                }
            }
            Check(matches==1,"missing/ambiguous identity");
        }
    }
    auto checkRejected=[](std::vector<glm::vec4> data,std::vector<HitKey> keys){
        const auto original=data;
        Check(!AppendIndex(data,2,keys),"invalid index accepted");
        Check(data==original,"rejected input mutated");
    };
    checkRejected(Fixture(2),{{0,0,0,0},{0,0,0,1}});
    checkRejected(Fixture(2),{{0,0,0,0},{0,0,1,0}});
    checkRejected(Fixture(2),{{0,0,0,0},{0,0,1,2}});
    checkRejected(Fixture(2),{{0,0,0,0}});
    auto bad=Fixture(2);bad[1].w=0;checkRejected(bad,{{0,0,0,0},{0,0,1,1}});
    bad=Fixture(2);bad[2].x=INFINITY;
    Check(!AppendIndex(bad,2,{{0,0,0,0},{0,0,1,1}}),"nonfinite geometry accepted");
    std::vector<glm::vec4> empty(1,glm::vec4(0));Check(AppendIndex(empty,0,{}),"empty scene");
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
    std::cout<<"PASS: emitter identity, CDF preservation, invalid inputs and VOX primitive corner mapping\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
