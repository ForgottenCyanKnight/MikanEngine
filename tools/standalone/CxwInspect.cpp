#include "Rendering/CxwLoader.h"
#include <iostream>
#include <stdexcept>
#include <set>
int main(int argc,char** argv){
    if((argc==4||argc==5)&&std::string(argv[1])=="--convert"){
        if(argc==5&&std::string(argv[4])!="--uniform")return 2;
        return CxwFormat::ConvertToVoxmesh(argv[2],argv[3],argc==5)?0:1;
    }
    if(argc<2||argc>3||(argc==3&&std::string(argv[2])!="--uniform")){std::cerr<<"usage: MikanCxwInspect file.cxw [--uniform]\n";return 2;}
    try{
        uint64_t meshes=0,emissiveQuads=0,maxPalette=0,mixedQuads=0;uint32_t firstColor=0,firstMaterial=0;std::set<uint32_t> decodedColors;
        const auto s=CxwFormat::Load(argv[1],[&](CxwFormat::Mesh&& mesh){
            uint64_t area=0;if(mesh.palette.size()>maxPalette)maxPalette=mesh.palette.size();
            size_t quadIndex=0;
            for(const auto& q:mesh.quads){
                if(q.direction>=6||q.palette>=mesh.palette.size())throw std::runtime_error("Invalid surface descriptor");
                area+=uint64_t((q.geometry>>24)+1)*((q.appearance&255)+1);
                emissiveQuads+=(q.material&2u)!=0;
                if(!meshes&&area==uint64_t((q.geometry>>24)+1)*((q.appearance&255)+1)){firstColor=q.appearance&0xffffff00u;firstMaterial=q.material;}
                if(mesh.palette[q.palette][0]!=(q.appearance&0xffffff00u)||mesh.palette[q.palette][1]!=q.material)
                    throw std::runtime_error("Surface material mismatch");
                const auto descriptor=mesh.descriptors.at(quadIndex++);
                if(descriptor&0x80000000u){++mixedQuads;
                    const auto offset=descriptor&0x3fffffffu;
                    const bool wide=(descriptor&0x40000000u)!=0u;
                    if(wide&&offset%2)throw std::runtime_error("Invalid R16 block");
                    const size_t cells=size_t((q.geometry>>24)+1)*((q.appearance&255)+1);
                    for(size_t i=0;i<cells;++i){const auto byte=offset+i*(wide?2:1);const auto id=(mesh.indexWords.at(byte/4)>>((byte%4)*8))&(wide?65535u:255u);
                        if(id>=mesh.palette.size())throw std::runtime_error("R16 palette index out of range");decodedColors.insert(mesh.palette[id][0]);}
                }else {if(descriptor!=q.palette)throw std::runtime_error("Uniform descriptor mismatch");decodedColors.insert(mesh.palette[descriptor][0]);}
            }
            if(area!=mesh.exposedFaces+mesh.hiddenFaces)throw std::runtime_error("Surface coverage mismatch");++meshes;
        },{},argc!=3);
        std::cout<<"{\"version\":"<<s.version<<",\"entities\":"<<s.entities<<",\"sectors\":"<<s.sectors
            <<",\"bricks\":"<<s.bricks<<",\"voxels\":"<<s.voxels<<",\"quads\":"<<s.quads
            <<",\"exposed_faces\":"<<s.exposedFaces<<",\"meshes\":"<<meshes
            <<",\"max_palette\":"<<maxPalette<<",\"emissive_quads\":"<<emissiveQuads<<",\"first_color\":"<<firstColor<<",\"first_material\":"<<firstMaterial
            <<",\"has_player_pose\":"<<(s.hasPlayerPose?"true":"false")
            <<",\"mixed_quads\":"<<mixedQuads<<",\"decoded_colors\":"<<decodedColors.size()
            <<",\"hidden_faces\":"<<s.hiddenFaces<<",\"greedy_quads\":"<<s.greedyQuads
            <<",\"center\":["<<s.center.x<<','<<s.center.y<<','<<s.center.z<<"]}\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
