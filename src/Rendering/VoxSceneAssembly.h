#pragma once
#include "Rendering/VoxLoader.h"
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace VoxFormat {
// Assemble the static first frame before meshing, so all rendering consumers see
// the same geometry. The compact quad encoding supports a 256-cell scene extent.
inline bool AssembleVoxScene(const std::vector<unsigned char>& bytes, VoxData& data,
                             std::string& error) {
    using Dict = std::map<std::string, std::string>;
    struct Reader {
        const unsigned char* p; size_t left;
        int integer() { if(left<4)throw std::runtime_error("Truncated scene chunk");
            int v;std::memcpy(&v,p,4);p+=4;left-=4;return v; }
        std::string string() {int n=integer();if(n<0 || size_t(n)>left)throw std::runtime_error("Invalid scene string");
            std::string v(reinterpret_cast<const char*>(p),n);p+=n;left-=n;return v;}
        Dict dict() {Dict d;int n=integer();if(n<0 || size_t(n)>left/8)throw std::runtime_error("Invalid scene dictionary");
            while(n--){auto k=string();d[k]=string();}return d;}
    };
    struct Transform {
        std::array<std::array<int,3>,3> r{{{1,0,0},{0,1,0},{0,0,1}}};
        std::array<int,3> t{};
    };
    struct Node {std::string type;Dict attrs;Transform transform;int layer=-1;
        std::vector<int> children;std::vector<std::pair<int,Dict>> models;};
    auto hidden=[](const Dict& d){auto i=d.find("_hidden");return i!=d.end() && i->second=="1";};
    try {
        std::map<int,Node> nodes;std::set<int> referenced, hiddenLayers;
        if(bytes.size()<20)throw std::runtime_error("Truncated VOX header");
        size_t offset=20;
        while(offset+12<=bytes.size()) {
            std::string type(reinterpret_cast<const char*>(bytes.data()+offset),4);
            uint32_t content,children;std::memcpy(&content,bytes.data()+offset+4,4);
            std::memcpy(&children,bytes.data()+offset+8,4);offset+=12;
            if(size_t(content)+children>bytes.size()-offset)throw std::runtime_error("Truncated VOX chunk");
            Reader reader{bytes.data()+offset,content};
            if(type=="LAYR") {int id=reader.integer();if(hidden(reader.dict()))hiddenLayers.insert(id);}
            else if(type=="nTRN" || type=="nGRP" || type=="nSHP") {
                int id=reader.integer();Node node;node.type=type;node.attrs=reader.dict();
                if(type=="nTRN") {
                    int child=reader.integer();node.children.push_back(child);referenced.insert(child);
                    reader.integer();node.layer=reader.integer();int count=reader.integer();
                    if(count<=0 || size_t(count)>reader.left/4)throw std::runtime_error("Invalid transform frames");
                    Dict frame;int best=2147483647;
                    while(count--){auto d=reader.dict();int f=d.count("_f")?std::stoi(d.at("_f")):0;
                        if(f<best){best=f;frame=std::move(d);}}
                    if(frame.count("_t")){std::istringstream stream(frame.at("_t"));
                        if(!(stream>>node.transform.t[0]>>node.transform.t[1]>>node.transform.t[2]))throw std::runtime_error("Invalid translation");}
                    if(frame.count("_r")) {
                        int rotation=std::stoi(frame.at("_r"));int a=rotation&3,b=(rotation>>2)&3;
                        if(a>2 || b>2 || a==b)throw std::runtime_error("Invalid rotation");
                        node.transform.r={};int axes[3]{a,b,3-a-b};
                        for(int row=0;row<3;++row)node.transform.r[row][axes[row]]=(rotation&(1<<(4+row)))?-1:1;
                    }
                    if(hidden(frame))node.attrs["_hidden"]="1";
                } else if(type=="nGRP") {
                    int count=reader.integer();if(count<0 || size_t(count)>reader.left/4)throw std::runtime_error("Invalid group size");
                    while(count--){int child=reader.integer();node.children.push_back(child);referenced.insert(child);}
                } else {
                    int count=reader.integer();if(count<=0 || size_t(count)>reader.left/8)throw std::runtime_error("Invalid shape size");
                    while(count--){int model=reader.integer();node.models.emplace_back(model,reader.dict());}
                }
                if(!nodes.emplace(id,std::move(node)).second)throw std::runtime_error("Duplicate scene node");
            }
            offset+=size_t(content)+children;
        }
        if(nodes.empty()) {
            if(data.models.size()>1)throw std::runtime_error("Multiple models without a scene graph cannot be positioned");
            return true;
        }
        std::map<std::array<int,3>,uint8_t> occupied;std::set<int> active;
        std::function<void(int,const Transform&)> visit=[&](int id,const Transform& parent) {
            auto found=nodes.find(id);if(found==nodes.end())throw std::runtime_error("Missing scene node");
            if(!active.insert(id).second)throw std::runtime_error("Scene graph cycle");
            const Node& node=found->second;
            if(hidden(node.attrs) || hiddenLayers.count(node.layer)){active.erase(id);return;}
            Transform world=parent;
            if(node.type=="nTRN") {
                world.r={};world.t=parent.t;
                for(int i=0;i<3;++i)for(int j=0;j<3;++j){world.t[i]+=parent.r[i][j]*node.transform.t[j];
                    for(int k=0;k<3;++k)world.r[i][k]+=parent.r[i][j]*node.transform.r[j][k];}
            }
            if(node.type=="nSHP") {
                const std::pair<int,Dict>* selected=nullptr;int best=2147483647;
                for(const auto& entry:node.models){int f=entry.second.count("_f")?std::stoi(entry.second.at("_f")):0;
                    if(f<best){best=f;selected=&entry;}}
                int index=selected->first;if(index<0 || size_t(index)>=data.models.size())throw std::runtime_error("Invalid model reference");
                if(!hidden(selected->second)) {
                    const auto& model=data.models[index];
                    for(const auto& voxel:model.voxels) {
                        if(!voxel.colorIndex)continue;
                        std::array<int,3> local{int(voxel.x)-int(model.sizeX/2),int(voxel.y)-int(model.sizeY/2),int(voxel.z)-int(model.sizeZ/2)};
                        auto position=world.t;
                        for(int i=0;i<3;++i)for(int j=0;j<3;++j){position[i]+=world.r[i][j]*local[j];
                            if(world.r[i][j]<0)--position[i];} // Rotate cell corners, not point samples.
                        occupied[position]=voxel.colorIndex;
                    }
                }
            }
            for(int child:node.children)visit(child,world);
            active.erase(id);
        };
        bool hasRoot=false;for(const auto& entry:nodes)if(!referenced.count(entry.first)){hasRoot=true;visit(entry.first,Transform{});}
        if(!hasRoot)throw std::runtime_error("Scene graph has no root");
        if(occupied.empty())throw std::runtime_error("Scene has no visible voxels");
        auto minimum=occupied.begin()->first,maximum=minimum;
        for(const auto& entry:occupied)for(int i=0;i<3;++i){minimum[i]=std::min(minimum[i],entry.first[i]);maximum[i]=std::max(maximum[i],entry.first[i]);}
        for(int i=0;i<3;++i)if(int64_t(maximum[i])-minimum[i]>=256)throw std::runtime_error("VOX scene exceeds compact mesh 256-cell extent");
        Model merged{};merged.sizeX=maximum[0]-minimum[0]+1;merged.sizeY=maximum[1]-minimum[1]+1;merged.sizeZ=maximum[2]-minimum[2]+1;
        merged.voxels.reserve(occupied.size());
        for(const auto& entry:occupied)merged.voxels.push_back({uint8_t(entry.first[0]-minimum[0]),uint8_t(entry.first[1]-minimum[1]),uint8_t(entry.first[2]-minimum[2]),entry.second});
        data.models.clear();data.models.push_back(std::move(merged));return true;
    } catch(const std::exception& exception) {error=exception.what();return false;}
}
} // namespace VoxFormat
