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
        if(mikan::rt::SelectVoxLod(10,0)!=0||mikan::rt::SelectVoxLod(100,0)!=2||mikan::rt::SelectVoxLod(200,0)!=3||mikan::rt::SelectVoxLod(78,2)!=2||mikan::rt::SelectVoxLod(30,1)!=0)throw std::runtime_error("LOD hysteresis test failed");
        VoxFormat::VoxData data;VoxFormat::Model model{};model.sizeX=model.sizeY=model.sizeZ=16;
        for(uint8_t z=0;z<16;++z)for(uint8_t y=0;y<16;++y)for(uint8_t x=0;x<16;++x)if(((x+y+z)%3)==0)model.voxels.push_back({x,y,z,1});
        data.models.push_back(model);CpuLodSource source;source.Bake(data);auto first=mikan::rt::LoadOrBuildVoxLod(source);auto cached=mikan::rt::LoadOrBuildVoxLod(source);
        if(!first->candidate||!cached->cacheHit)throw std::runtime_error("LOD cache self-test failed");
        uint32_t previous=first->originalTriangles,valid=0;for(int l=0;l<3;++l){const auto count=first->levels[l].triangles.size();if(count&&(count>=previous||count>first->originalTriangles/(std::array<uint32_t,3>{4u,20u,100u}[l])))throw std::runtime_error("LOD budget guard failed");if(count){previous=uint32_t(count);++valid;}
            if(count!=cached->levels[l].triangles.size())throw std::runtime_error("LOD cached count mismatch");}
        if(valid!=3)throw std::runtime_error("test fixture did not produce all three simplified LODs");
        if(const auto* directory=std::getenv("MIKAN_VOX_LOD_CACHE")){std::ostringstream name;name<<std::hex<<std::setfill('0')<<std::setw(16)<<first->key<<".vlod";
            const auto file=std::filesystem::u8path(directory)/name.str();{std::ofstream corrupt(file,std::ios::binary|std::ios::trunc);corrupt<<"corrupt";}
            const auto regenerated=mikan::rt::LoadOrBuildVoxLod(source);if(regenerated->cacheHit||regenerated->levels[0].triangles.empty())throw std::runtime_error("corrupt cache fallback failed");}
        std::cout<<"Vox remesh LOD self-tests PASS\n";return 0;
    }
    if(argc!=3)throw std::runtime_error("usage: MikanVoxLodBake source.vox report.json | --self-test");
    VoxFormat::VoxData source;if(!VoxFormat::LoadVoxFile(argv[1],source))throw std::runtime_error("VOX load failed");
    std::vector<uint32_t> uses(source.models.size());if(source.instances.empty())uses[0]=1;else for(const auto& instance:source.instances)++uses.at(instance.modelIndex);
    nlohmann::json rows=nlohmann::json::array();uint32_t candidates=0,hits=0;uint64_t original=0,lodTriangles[3]{};
    for(uint32_t i=0;i<source.models.size();++i){if(!uses[i])continue;VoxFormat::VoxData leaf;leaf.models.push_back(source.models[i]);leaf.hasMaterials=source.hasMaterials;leaf.hasCustomPalette=source.hasCustomPalette;
        std::copy(std::begin(source.palette),std::end(source.palette),std::begin(leaf.palette));std::copy(std::begin(source.materials),std::end(source.materials),std::begin(leaf.materials));
        CpuLodSource renderer;renderer.Bake(leaf);auto data=mikan::rt::LoadOrBuildVoxLod(renderer);if(!data->candidate)continue;++candidates;hits+=data->cacheHit;original+=uint64_t(data->originalTriangles)*uses[i];
        for(uint32_t l=0;l<3;++l){const auto level=data->AvailableLevel(l+1);lodTriangles[l]+=uint64_t(level?data->levels[level-1].triangles.size():data->originalTriangles)*uses[i];}
        rows.push_back({{"model_index",i},{"instances",uses[i]},{"original_triangles",data->originalTriangles},{"resolution",data->resolution},{"remesh_triangles",data->remeshTriangles},{"lod_triangles",{data->levels[0].triangles.size(),data->levels[1].triangles.size(),data->levels[2].triangles.size()}},{"cache_hit",data->cacheHit}});
    }
    nlohmann::json report={{"candidates",candidates},{"cache_hits",hits},{"candidate_instance_triangles",original},{"candidate_lod_instance_triangles",{lodTriangles[0],lodTriangles[1],lodTriangles[2]}},{"components",rows}};
    std::ofstream out(Utf8Path(argv[2]));out<<report.dump(2);if(!out)throw std::runtime_error("report write failed");std::cout<<"LOD candidates="<<candidates<<" cache hits="<<hits<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
