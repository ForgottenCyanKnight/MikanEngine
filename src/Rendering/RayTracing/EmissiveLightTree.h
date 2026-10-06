#pragma once
// Receiver-dependent NEE proposal. Independent implementation; no ReSTIR state.
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <vector>

namespace mikan::rt::lights {
constexpr uint32_t SelectorCount=1u<<24,LeafBit=1u<<31;
struct HitKey {uint32_t instance,geometry,primitive,emitter;};
inline bool Less(const HitKey& a,const HitKey& b){
    if(a.instance!=b.instance)return a.instance<b.instance;
    if(a.geometry!=b.geometry)return a.geometry<b.geometry;
    return a.primitive<b.primitive;
}
struct Node {glm::vec3 center{};float mass=0;uint32_t child=0,leaves=0,parent=UINT32_MAX;float radius2=0;};
struct Tree {std::vector<Node> nodes;std::vector<uint32_t> leafNodes;};
inline std::array<glm::vec3,4> VoxCorners(uint32_t g,uint32_t appearance,uint32_t direction,
    float voxelSize,glm::vec3 minBounds,const glm::mat4& model){
    static const glm::vec2 uv[6][4]={
        {{0,0},{1,0},{1,1},{0,1}},{{0,0},{0,1},{1,1},{1,0}},
        {{1,0},{1,1},{0,1},{0,0}},{{0,0},{0,1},{1,1},{1,0}},
        {{0,0},{0,1},{1,1},{1,0}},{{0,1},{0,0},{1,0},{1,1}}};
    glm::vec3 origin(float(g&255u),float((g>>8)&255u),float((g>>16)&255u));
    if(direction==0)origin.z+=1;else if(direction==3)origin.x+=1;else if(direction==4)origin.y+=1;
    std::array<glm::vec3,4> corners;
    for(uint32_t c=0;c<4;++c){const auto offset=uv[direction][c]*glm::vec2(float((g>>24)+1),float((appearance&255u)+1));
        const glm::vec3 grid=origin+(direction<2?glm::vec3(offset,0):
            (direction<4?glm::vec3(0,offset.y,offset.x):glm::vec3(offset.x,0,offset.y)));
        corners[c]=glm::vec3(model*glm::vec4(grid*voxelSize+minBounds,1));}
    return corners;
}
inline bool Build(const std::vector<glm::vec4>& lights,uint32_t count,Tree& tree,bool powerCentroid=false){
    tree={};if(!count)return true;
    if(count>SelectorCount || lights.size()<1ull+4ull*count)return false;
    std::vector<double> mass(count);double totalArea=0,totalPower=0;
    for(uint32_t i=0;i<count;++i){const auto& l=lights[1+4*i];
        if(!std::isfinite(l.w)||l.w<=0)return false;
        double power=double(l.w)*glm::dot(glm::max(glm::vec3(l),glm::vec3(0)),glm::vec3(.2126f,.7152f,.0722f));
        if(!std::isfinite(power))return false;
        mass[i]=power;totalArea+=l.w;totalPower+=power;
        for(uint32_t k=1;k<=3;++k)for(uint32_t a=0;a<3;++a)
            if(!std::isfinite(lights[1+4*i+k][a]))return false;
    }
    for(uint32_t i=0;i<count;++i)mass[i]=totalPower>0?.9*mass[i]/totalPower+.1*lights[1+4*i].w/totalArea:lights[1+4*i].w/totalArea;
    std::vector<uint32_t> order(count);std::iota(order.begin(),order.end(),0u);
    tree.nodes.resize(1);tree.nodes.reserve(2ull*count-1);tree.leafNodes.resize(count);
    std::function<bool(uint32_t,uint32_t,uint32_t,uint32_t)> build=[&](uint32_t at,uint32_t begin,uint32_t end,uint32_t parent){
        glm::vec3 lo(INFINITY),hi(-INFINITY);double sum=0;glm::dvec3 weighted(0);
        for(uint32_t j=begin;j<end;++j){uint32_t i=order[j];sum+=mass[i];
            if(powerCentroid)weighted+=(glm::dvec3(lights[2+4*i])+glm::dvec3(lights[3+4*i])+glm::dvec3(lights[4+4*i]))*(mass[i]/3.);
            for(uint32_t k=1;k<=3;++k){const glm::vec3 p(lights[1+4*i+k]);lo=glm::min(lo,p);hi=glm::max(hi,p);}}
        Node node;node.center=lo*.5f+hi*.5f;node.mass=float(sum);node.leaves=end-begin;node.parent=parent;
        if(powerCentroid)node.center=glm::vec3(weighted/sum);
        const glm::vec3 radius=hi*.5f-lo*.5f;node.radius2=glm::dot(radius,radius);
        if(!std::isfinite(node.radius2)||!std::isfinite(node.mass)||node.mass<=0)return false;
        if(end-begin==1){node.child=LeafBit|order[begin];tree.leafNodes[order[begin]]=at;tree.nodes[at]=node;return true;}
        const glm::vec3 extent=hi-lo;uint32_t axis=extent.y>extent.x?1u:0u;if(extent.z>extent[axis])axis=2;
        const uint32_t mid=begin+(end-begin)/2;
        auto center=[&](uint32_t i){return (double(lights[2+4*i][axis])+lights[3+4*i][axis]+lights[4+4*i][axis])/3.;};
        std::nth_element(order.begin()+begin,order.begin()+mid,order.begin()+end,[&](uint32_t a,uint32_t b){const double x=center(a),y=center(b);return x==y?a<b:x<y;});
        node.child=uint32_t(tree.nodes.size());tree.nodes.resize(tree.nodes.size()+2);tree.nodes[at]=node;
        return build(node.child,begin,mid,at)&&build(node.child+1,mid,end,at);
    };
    return build(0,0,count,UINT32_MAX);
}
// Exactly the represented integer probability used by the shader, including support floors.
inline uint32_t LeftCount(const Node& a,const Node& b,const glm::vec3& point,uint32_t count){
    const auto da=a.center-point,db=b.center-point;
    const float dA=std::max({glm::dot(da,da),a.radius2,1e-12f}),dB=std::max({glm::dot(db,db),b.radius2,1e-12f});
    const float scale=std::max(dA,dB),powerScale=std::max(a.mass,b.mass);
    const float wa=(a.mass/powerScale)*(dB/scale),wb=(b.mass/powerScale)*(dA/scale);
    const float p=std::clamp(wa/(wa+wb),1e-6f,1.f-1e-6f);
    return std::clamp(uint32_t(std::floor(float(count)*p+.5f)),a.leaves,count-b.leaves);
}
inline float Bits(uint32_t word){return std::bit_cast<float>(word);}
inline bool Append(std::vector<glm::vec4>& lights,uint32_t count,std::vector<HitKey> keys,bool powerCentroid=false){
    Tree tree;if(keys.size()!=count||!Build(lights,count,tree,powerCentroid))return false;
    if(!count)return true;
    std::vector<bool> indexed(count,false);
    for(const auto& key:keys){if(key.emitter>=count||indexed[key.emitter])return false;indexed[key.emitter]=true;}
    std::sort(keys.begin(),keys.end(),Less);
    for(size_t i=1;i<keys.size();++i)if(!Less(keys[i-1],keys[i]))return false;
    if(lights.size()+tree.nodes.size()*2+count*2>UINT32_MAX)return false;
    const uint32_t nodeOffset=uint32_t(lights.size());
    for(const auto& n:tree.nodes){lights.emplace_back(n.center,n.mass);lights.emplace_back(Bits(n.child),Bits(n.leaves),Bits(n.parent),n.radius2);}
    const uint32_t leafOffset=uint32_t(lights.size());
    for(auto at:tree.leafNodes)lights.emplace_back(Bits(at),0,0,0);
    const uint32_t keyOffset=uint32_t(lights.size());
    for(const auto& k:keys)lights.emplace_back(Bits(k.instance),Bits(k.geometry),Bits(k.primitive),Bits(k.emitter));
    lights[0].y=Bits(nodeOffset);lights[0].z=Bits(leafOffset);lights[0].w=Bits(keyOffset);
    return true;
}
}
