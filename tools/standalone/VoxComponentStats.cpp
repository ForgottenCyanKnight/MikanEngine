#include "Rendering/VoxRenderer.h"
#include "Core/Log.h"
#include "Core/Utf8Path.h"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
// Production exterior extraction and contact backfill, without Init or GPU upload.
struct CpuSurface final : VoxRenderer {
    void Bake(const VoxFormat::VoxData& data) {
        if(!data.models.empty()&&!data.models[0].voxels.empty())BuildVoxelFaces(data,1.f);
    }
};
struct Thresholds {uint64_t minimumTriangles=10000;double maximumMergeFactor=4.;};
bool Candidate(const VoxSurfaceReductionStats& stats,uint64_t instances,const Thresholds& t) {
    return instances&&stats.TriangleCount()>=t.minimumTriangles&&stats.MergeFactor()<=t.maximumMergeFactor;
}
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void SelfTest(){
    Thresholds t{10,2};
    Require(!Candidate({6,6,6},0,t),"unused component selected");
    Require(!Candidate({6,1,1},1,t),"tiny high-reduction component selected");
    Require(Candidate({12,6,6},1,t),"inclusive thresholds changed");
    Require(!Candidate({13,6,6},1,t),"merge threshold ignored");
    VoxFormat::VoxData data;VoxFormat::Model model{};model.sizeX=model.sizeY=model.sizeZ=2;
    for(uint8_t z=0;z<2;++z)for(uint8_t y=0;y<2;++y)for(uint8_t x=0;x<2;++x)model.voxels.push_back({x,y,z,1});
    data.models.push_back(model);CpuSurface surface;surface.Bake(data);
    const auto s=surface.GetSurfaceReductionStats();
    Require(s.exposedFaceCount==24&&s.finalQuadCount==6&&s.TriangleCount()==12,"production cube counts changed");
    Require(std::abs(s.MergeFactor()-4.)<1e-9,"production merge factor changed");
    data.models[0].voxels.clear();CpuSurface empty;empty.Bake(data);
    Require(empty.GetSurfaceReductionStats().exposedFaceCount==0,"empty model stats changed");
    std::cout<<"Vox component stats: threshold and production bake tests PASS\n";
}
}
int main(int argc,char** argv){
    try{
        Core::SetLogFileEnabled(false);
        if(argc==2&&std::string(argv[1])=="--self-test"){SelfTest();return 0;}
        Require(argc>=4,"usage: MikanVoxComponentStats source.vox --output prefix [--min-triangles 10000] [--max-merge-factor 4]");
        const auto input=std::filesystem::absolute(Utf8Path(argv[1]));std::string output;Thresholds threshold;
        for(int a=2;a<argc;++a){const std::string option=argv[a];Require(a+1<argc,"option needs a value");
            if(option=="--output")output=argv[++a];
            else if(option=="--min-triangles")threshold.minimumTriangles=std::stoull(argv[++a]);
            else if(option=="--max-merge-factor")threshold.maximumMergeFactor=std::stod(argv[++a]);
            else throw std::runtime_error("unknown option: "+option);
        }
        Require(!output.empty()&&threshold.minimumTriangles>0&&std::isfinite(threshold.maximumMergeFactor)&&threshold.maximumMergeFactor>=1.,"invalid output or thresholds");
        const auto prefix=std::filesystem::absolute(Utf8Path(output));if(!prefix.parent_path().empty())std::filesystem::create_directories(prefix.parent_path());
        VoxFormat::VoxData source;Require(VoxFormat::LoadVoxFile(Utf8String(input),source)&&!source.models.empty(),"source VOX load failed");
        std::vector<uint64_t> uses(source.models.size());std::map<uint32_t,uint32_t> component;
        if(source.instances.empty()){uses[0]=1;component[0]=0;}
        else for(const auto& instance:source.instances){Require(instance.modelIndex<uses.size(),"invalid model instance");++uses[instance.modelIndex];
            if(!component.contains(instance.modelIndex))component.emplace(instance.modelIndex,uint32_t(component.size()));}
        nlohmann::json rows=nlohmann::json::array();uint64_t uniqueTriangles=0,weightedTriangles=0,weightedFaces=0,weightedQuads=0,proxyTriangles=0,proxyWeighted=0,proxyCount=0;
        for(uint32_t index=0;index<source.models.size();++index){
            VoxFormat::VoxData leaf;std::copy(std::begin(source.palette),std::end(source.palette),std::begin(leaf.palette));
            std::copy(std::begin(source.materials),std::end(source.materials),std::begin(leaf.materials));leaf.hasCustomPalette=source.hasCustomPalette;leaf.hasMaterials=source.hasMaterials;
            leaf.models.push_back(source.models[index]);CpuSurface surface;surface.Bake(leaf);
            const auto s=surface.GetSurfaceReductionStats();const auto triangles=s.TriangleCount();const bool candidate=Candidate(s,uses[index],threshold);
            const auto extent=surface.GetVoxelCount()?surface.GetMaxBounds()-surface.GetMinBounds():glm::vec3(0);const double volume=double(extent.x)*extent.y*extent.z;
            nlohmann::json row={{"model_index",index},{"runtime_component",component.contains(index)?int64_t(component.at(index)):-1},{"instances",uses[index]},
                {"voxels",surface.GetVoxelCount()},{"occupied_extent_xyz",{extent.x,extent.y,extent.z}},
                {"occupancy",volume>0?double(surface.GetVoxelCount())/volume:0.},{"exposed_unit_faces",s.exposedFaceCount},
                {"greedy_quads",s.greedyQuadCount},{"final_quads",s.finalQuadCount},{"triangles",triangles},
                {"merge_factor",s.MergeFactor()},{"reduction_percent",s.ReductionFraction()*100.},
                {"backfill_reduction_percent",s.greedyQuadCount?100.*(1.-double(s.finalQuadCount)/s.greedyQuadCount):0.},
                {"surface_attribute_bytes",surface.GetSurfaceAttributes().size()*4},{"instance_weighted_triangles",triangles*uses[index]},
                {"proxy_candidate",candidate},{"small_component",uses[index]>0&&triangles<=1024}};
            rows.push_back(std::move(row));
            if(uses[index])uniqueTriangles+=triangles;weightedTriangles+=triangles*uses[index];weightedFaces+=s.exposedFaceCount*uses[index];weightedQuads+=s.finalQuadCount*uses[index];
            if(candidate){++proxyCount;proxyTriangles+=triangles;proxyWeighted+=triangles*uses[index];}
            if(index%32==0)std::cerr<<"components "<<index+1<<'/'<<source.models.size()<<'\n';
        }
        for(auto& row:rows)row["instance_triangle_share_percent"]=weightedTriangles?100.*row["instance_weighted_triangles"].get<uint64_t>()/weightedTriangles:0.;
        std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.at("instance_weighted_triangles").template get<uint64_t>()>b.at("instance_weighted_triangles").template get<uint64_t>();});
        nlohmann::json summary={{"source_models",source.models.size()},{"used_components",component.size()},
            {"instances",source.instances.empty()?1:source.instances.size()},{"component_triangles",uniqueTriangles},{"instance_weighted_triangles",weightedTriangles},
            {"instance_weighted_exposed_faces",weightedFaces},{"instance_weighted_final_quads",weightedQuads},
            {"weighted_merge_factor",weightedQuads?double(weightedFaces)/weightedQuads:0.},
            {"proxy_candidates",proxyCount},{"proxy_component_triangles",proxyTriangles},{"proxy_instance_weighted_triangles",proxyWeighted},
            {"proxy_instance_triangle_share_percent",weightedTriangles?100.*proxyWeighted/weightedTriangles:0.}};
        nlohmann::json document={{"schema_version",1},{"source",Utf8String(input)},{"mode","cpu_production_component_bake"},
            {"scope","source components for default hardware triangles; DAG off; physical BLAS deduplication and GPU traversal time are not measured"},
            {"thresholds",{{"minimum_triangles",threshold.minimumTriangles},{"maximum_merge_factor",threshold.maximumMergeFactor}}},{"summary",summary},{"components",rows}};
        std::ofstream json(Utf8Path(Utf8String(prefix)+".json"));json<<document.dump(2)<<'\n';Require(json.good(),"JSON write failed");
        std::ofstream csv(Utf8Path(Utf8String(prefix)+".csv"));
        const std::vector<std::string> columns={"model_index","runtime_component","instances","voxels","exposed_unit_faces","greedy_quads","final_quads","triangles","merge_factor","reduction_percent","backfill_reduction_percent","occupancy","surface_attribute_bytes","instance_weighted_triangles","instance_triangle_share_percent","proxy_candidate","small_component"};
        for(size_t c=0;c<columns.size();++c)csv<<(c?",":"")<<columns[c];csv<<'\n';
        for(const auto& row:rows){for(size_t c=0;c<columns.size();++c)csv<<(c?",":"")<<row.at(columns[c]).dump();csv<<'\n';}Require(csv.good(),"CSV write failed");
        std::cout<<summary.dump(2)<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
