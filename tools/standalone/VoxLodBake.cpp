#include "Rendering/VoxRenderer.h"
#include "Rendering/RayTracing/VoxLod.h"
#include "Core/Log.h"
#include "Core/Utf8Path.h"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <iomanip>
struct CpuLodSource final:VoxRenderer {void Bake(const VoxFormat::VoxData& data){if(!data.models[0].voxels.empty())BuildVoxelFaces(data,1);}};
int main(int argc,char** argv){try{
    Core::SetLogFileEnabled(false);
    if(argc==2&&std::string(argv[1])=="--self-test"){
        if(mikan::rt::SelectVoxLod(10,0)!=0||mikan::rt::SelectVoxLod(100,0)!=1||mikan::rt::SelectVoxLod(200,0)!=1||mikan::rt::SelectVoxLod(38,1)!=1||mikan::rt::SelectVoxLod(38,0)!=0||mikan::rt::SelectVoxLod(30,1)!=0||mikan::rt::SelectVoxLod(100,99)!=1)throw std::runtime_error("LOD hysteresis test failed");
        VoxFormat::VoxData data;VoxFormat::Model model{};model.sizeX=model.sizeY=model.sizeZ=16;
        for(uint8_t z=0;z<16;++z)for(uint8_t y=0;y<16;++y)for(uint8_t x=0;x<16;++x)if(((x+y+z)%3)==0)model.voxels.push_back({x,y,z,1});
        data.models.push_back(model);CpuLodSource source;source.Bake(data);auto first=mikan::rt::LoadOrBuildVoxLod(source);auto cached=mikan::rt::LoadOrBuildVoxLod(source);
        if(!first->candidate||!cached->cacheHit)throw std::runtime_error("LOD cache self-test failed");
        if(first->levels[0].triangles.empty()||!first->levels[1].triangles.empty()||!first->levels[2].triangles.empty()||first->AvailableLevel(99)!=1)
            throw std::runtime_error("two-level availability failed");
        if(first->levels[0].triangles.size()!=cached->levels[0].triangles.size())throw std::runtime_error("LOD cached count mismatch");
        // Efficient, tiny and special-material meshes must no longer be filtered.
        for(int material:{0,1,2,3,4}){
            VoxFormat::VoxData tiny;tiny.hasMaterials=material!=0;tiny.materials[1].type=material;tiny.materials[1].rough=.3f;tiny.materials[1].metallic=.4f;
            VoxFormat::Model cube{};cube.sizeX=cube.sizeY=cube.sizeZ=4;
            for(uint8_t z=0;z<4;++z)for(uint8_t y=0;y<4;++y)for(uint8_t x=0;x<4;++x)cube.voxels.push_back({x,y,z,1});
            tiny.models.push_back(cube);CpuLodSource mesh;mesh.Bake(tiny);auto lod=mikan::rt::LoadOrBuildVoxLod(mesh);
            if(!lod->candidate||lod->levels[0].triangles.empty()||lod->originalTriangles!=12||lod->mergeFactor<=4)
                throw std::runtime_error("universal tiny/efficient/material LOD failed");
            const uint32_t flag=material==1?1u:material==2?8u:material==3?2u:material==4?4u:0u;
            for(const auto& triangle:lod->levels[0].triangles)if((triangle.material&15u)!=flag)throw std::runtime_error("proxy material lost");
            if(lod->hasEmission!=(material==3))throw std::runtime_error("LOD emission metadata mismatch");
        }
        VoxFormat::VoxData slope;VoxFormat::Model wedge{};wedge.sizeX=wedge.sizeY=wedge.sizeZ=8;
        for(uint8_t z=0;z<8;++z)for(uint8_t y=0;y<8;++y)for(uint8_t x=0;x<8;++x)if(z<=x)wedge.voxels.push_back({x,y,z,1});
        slope.models.push_back(wedge);CpuLodSource sloped;sloped.Bake(slope);const auto slopedLod=mikan::rt::LoadOrBuildVoxLod(sloped);
        if(!slopedLod->candidate||slopedLod->levels[0].triangles.empty()||slopedLod->AvailableLevel(1)!=1)throw std::runtime_error("slope LOD unavailable");
        if(const auto* directory=std::getenv("MIKAN_VOX_LOD_CACHE")){std::ostringstream name;name<<std::hex<<std::setfill('0')<<std::setw(16)<<first->key<<".vlod";
            const auto file=std::filesystem::u8path(directory)/name.str();{std::ofstream corrupt(file,std::ios::binary|std::ios::trunc);corrupt<<"corrupt";}
            const auto regenerated=mikan::rt::LoadOrBuildVoxLod(source);if(regenerated->cacheHit||regenerated->levels[0].triangles.empty())throw std::runtime_error("corrupt cache fallback failed");}
        std::cout<<"Vox universal two-level LOD self-tests PASS\n";return 0;
    }
    if(argc!=3)throw std::runtime_error("usage: MikanVoxLodBake source.vox report.json | --self-test");
    VoxFormat::VoxData source;if(!VoxFormat::LoadVoxFile(argv[1],source))throw std::runtime_error("VOX load failed");
    std::vector<uint32_t> uses(source.models.size());if(source.instances.empty())uses[0]=1;else for(const auto& instance:source.instances)++uses.at(instance.modelIndex);
    nlohmann::json rows=nlohmann::json::array();uint32_t candidates=0,hits=0;uint64_t original=0,lodTriangles[3]{};
    for(uint32_t i=0;i<source.models.size();++i){if(!uses[i])continue;VoxFormat::VoxData leaf;leaf.models.push_back(source.models[i]);leaf.hasMaterials=source.hasMaterials;leaf.hasCustomPalette=source.hasCustomPalette;
        std::copy(std::begin(source.palette),std::end(source.palette),std::begin(leaf.palette));std::copy(std::begin(source.materials),std::end(source.materials),std::begin(leaf.materials));
        CpuLodSource renderer;renderer.Bake(leaf);auto data=mikan::rt::LoadOrBuildVoxLod(renderer);if(!data->candidate)continue;++candidates;hits+=data->cacheHit;original+=uint64_t(data->originalTriangles)*uses[i];
        for(uint32_t l=0;l<mikan::rt::kVoxLodMaxLevel;++l){const auto level=data->AvailableLevel(l+1);lodTriangles[l]+=uint64_t(level?data->levels[level-1].triangles.size():data->originalTriangles)*uses[i];}
        rows.push_back({{"model_index",i},{"instances",uses[i]},{"original_triangles",data->originalTriangles},{"resolution",data->resolution},{"remesh_triangles",data->remeshTriangles},{"lod_triangles",{data->levels[0].triangles.size()}},{"cache_hit",data->cacheHit}});
    }
    nlohmann::json report={{"total_levels",2},{"candidate_policy","all_vox_blas"},{"candidates",candidates},{"cache_hits",hits},{"candidate_instance_triangles",original},{"candidate_lod_instance_triangles",{lodTriangles[0]}},{"components",rows}};
    std::ofstream out(Utf8Path(argv[2]));out<<report.dump(2);if(!out)throw std::runtime_error("report write failed");std::cout<<"LOD candidates="<<candidates<<" cache hits="<<hits<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
