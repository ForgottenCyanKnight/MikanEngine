#include "Core/ProjectManager.h"
#include "Core/Utf8Path.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "Supply a new fixture directory");
        const auto root = fs::absolute(Utf8Path(argv[1]));
        Require(!fs::exists(root), "Fixture must not already exist");
        const auto shared = root / Utf8Path("共享素材 space");
        fs::create_directories(shared / "textures");
        std::ofstream(shared / "model.glb") << "fixture";
        std::ofstream(shared / "textures" / "color.png") << "fixture";
        auto& manager = ProjectManager::GetInstance();
        for (const char* name : {"first", "second"}) {
            const auto project = root / name;
            fs::create_directories(project / "models");
            std::ofstream(project / "models" / "old.glb") << "existing";
            std::ofstream(project / "project.json") << R"({"formatVersion":1,"resourceRoot":".","assets":["models"]})";
            Require(manager.SetProjectRoot(Utf8String(project)), "Open project");
            Require(manager.IsProjectAsset(Utf8String(project / "models" / "old.glb"), false), "Legacy asset visibility");
            Require(manager.RegisterProjectAsset(Utf8String(shared / "model.glb")), "Link file");
            Require(manager.RegisterProjectAsset(Utf8String(shared)), "Link directory");
            Require(manager.ReloadManifest(), "Reload links");
            Require(manager.IsProjectAsset(Utf8String(shared / "textures" / "color.png"), false), "Directory descendant visibility");
            Require(!manager.IsProjectAsset(Utf8String(root / "unregistered.glb"), false), "Unregistered scope");
            Require(!fs::exists(project / "model.glb"), "No authoring copy");
            Require(manager.ResolveAssetPath(Utf8String(shared / "model.glb")) == Utf8String(shared / "model.glb"), "Resolve original source");
        }
        Require(manager.UnregisterProjectAsset(Utf8String(shared)), "Unlink directory");
        Require(!manager.IsProjectAsset(Utf8String(shared / "model.glb"), false), "Unlink descendants");
        Require(fs::exists(shared / "model.glb"), "Unlink preserves original");
        Require(manager.SetProjectRoot(Utf8String(root / "first")), "Reopen first project");
        Require(manager.IsProjectAsset(Utf8String(shared / "model.glb"), false), "Other project keeps link");
#if defined(__linux__) && !defined(__ANDROID__)
        const auto caseProject = root / "case-project";
        fs::create_directories(caseProject);
        std::ofstream(caseProject / "project.json") << R"({"formatVersion":1,"resourceRoot":".","assets":[]})";
        std::ofstream(caseProject / "Model.glb") << "upper";
        std::ofstream(caseProject / "model.glb") << "lower";
        Require(manager.SetProjectRoot(Utf8String(caseProject)), "Open case-sensitive project");
        Require(manager.RegisterProjectAsset(Utf8String(caseProject / "Model.glb")), "Link uppercase asset");
        Require(!manager.IsProjectAsset(Utf8String(caseProject / "model.glb"), false), "Case-distinct asset leaked into scope");
        Require(manager.RegisterProjectAsset(Utf8String(caseProject / "model.glb")), "Link lowercase asset");
        Require(manager.ReloadManifest(), "Reload case-distinct assets");
        Require(manager.UnregisterProjectAsset(Utf8String(caseProject / "Model.glb")), "Unlink uppercase asset");
        Require(manager.IsProjectAsset(Utf8String(caseProject / "model.glb"), false), "Unlink removed case-distinct asset");
#endif
        std::cout << "PASS: shared links, legacy resources, UTF-8, persistence and safe unlink\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
