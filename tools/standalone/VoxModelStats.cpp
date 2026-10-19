#include "VoxSurfaceValidation.h"

namespace {
using namespace VoxValidation;
void SelfTest() {
    auto run = [](const char* name, const std::vector<Cell>& cells, int colors, bool emissive = false, bool glass = false) {
        VoxFormat::VoxData data;
        VoxFormat::Model model{};
        model.sizeX = model.sizeY = model.sizeZ = 256;
        for (const auto& p : cells) model.voxels.push_back({uint8_t(p[0]),uint8_t(p[1]),uint8_t(p[2]),uint8_t(1+(p[0]+p[1]+p[2])%colors)});
        data.hasMaterials = emissive || glass;
        data.materials[1].type = emissive ? 3 : (glass ? 2 : 0);
        auto result = Analyze(data,model);
        std::cout << name << ": " << result.details.dump() << '\n';
        return result;
    };
    Require(run("empty",{},1).stats.exposedFaceCount == 0, "empty stats");
    const auto unit = run("single",{{11,17,23}},1);
    Require(unit.stats.exposedFaceCount == 6 && unit.stats.finalQuadCount == 6, "single cube stats");
    std::vector<Cell> solid;
    for(int x=11;x<13;++x)for(int y=17;y<19;++y)for(int z=23;z<25;++z)solid.push_back({x,y,z});
    const auto cube = run("solid_2x2x2",solid,1);
    Require(cube.stats.exposedFaceCount == 24 && cube.stats.finalQuadCount == 6, "solid cube reduction");
    Require(std::abs(cube.stats.ReductionFraction()-.75)<1e-12, "solid cube reduction fraction");
    run("multicolor",solid,2);
    run("emissive_barrier",solid,2,true);
    run("glass_barrier",solid,2,false,true);
    std::vector<Cell> contact;
    for(int x=11;x<19;++x)for(int z=23;z<31;++z)contact.push_back({x,17,z});
    for(int y=18;y<20;++y)for(int x=14;x<16;++x)for(int z=26;z<28;++z)contact.push_back({x,y,z});
    const auto backfill = run("contact_pillar",contact,2);
    Require(backfill.stats.finalQuadCount < backfill.stats.greedyQuadCount, "contact fixture did not exercise reduction");
    std::vector<Cell> shell;
    for(int x=0;x<5;++x)for(int y=0;y<5;++y)for(int z=0;z<5;++z)
        if(x==0||y==0||z==0||x==4||y==4||z==4)shell.push_back({x,y,z});
    run("hollow",shell,2);
    std::vector<Cell> boundary;
    for(int x=0;x<256;++x)boundary.push_back({x,255,255});
    Require(run("u8_boundary",boundary,1).stats.finalQuadCount==6,"256-cell boundary reduction");
    VoxSurfaceReductionStats aggregate = cube.stats;
    aggregate += cube.stats; aggregate += unit.stats;
    Require(aggregate.exposedFaceCount==54 && aggregate.finalQuadCount==18, "instance-weighted stats");
}
} // namespace

int main(int argc,char** argv) {
    using namespace VoxValidation;
    try {
        Core::SetLogFileEnabled(false);
        if(argc==2 && std::string(argv[1])=="--self-test") { SelfTest(); return 0; }
        nlohmann::json reports=nlohmann::json::array();
        std::string output;
        for(int arg=1;arg<argc;++arg) {
            if(std::string(argv[arg])=="--output") {
                Require(arg+1<argc,"--output requires a path");output=argv[++arg];continue;
            }
            const auto path=std::filesystem::absolute(Utf8Path(argv[arg]));
            VoxFormat::VoxData data;
            Require(VoxFormat::LoadVoxFile(Utf8String(path),data),"VOX loader failed");
            nlohmann::json models=nlohmann::json::array();
            std::vector<VoxSurfaceReductionStats> sourceStats;
            for(size_t i=0;i<data.models.size();++i) {
                auto result=Analyze(data,data.models[i]);result.details["model_index"]=i;
                models.push_back(result.details);sourceStats.push_back(result.stats);
            }
            VoxSurfaceReductionStats total;
            uint64_t voxels=0;
            if(data.instances.empty()) {
                // VoxRenderer uses model zero unless the loader supplied a composite.
                if(!sourceStats.empty()){total=sourceStats[0];voxels=data.models[0].voxels.size();}
            } else for(const auto& instance:data.instances) {
                total+=sourceStats.at(instance.modelIndex);voxels+=data.models.at(instance.modelIndex).voxels.size();
            }
            reports.push_back({{"file",Utf8String(path)}, {"name",Utf8String(path.filename())},
                {"models",models}, {"instances",data.instances.size()}, {"voxel_count",voxels},
                {"stats",StatsJson(total)}, {"validation","pass"}});
        }
        Require(!reports.empty(),"usage: MikanVoxModelStats model.vox [more.vox ...] [--output report.json] | --self-test");
        nlohmann::json document={{"schema_version",1},{"mode","cpu_production_surface_bake"},{"files",reports}};
        if(!output.empty()) {std::ofstream file(Utf8Path(output));file<<document.dump(2)<<'\n';Require(file.good(),"report write failed");}
        std::cout<<document.dump(2)<<'\n';
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
