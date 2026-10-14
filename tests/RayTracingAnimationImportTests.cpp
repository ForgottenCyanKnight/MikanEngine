#include "Rendering/ModelLoader.h"
#include "Rendering/RayTracing/RayTracingDeformation.h"
#include <iostream>
#include <cstdlib>
#include <cmath>

static void Require(bool value,const char* message){if(!value){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}}
int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"Usage: RayTracingAnimationImportTests <absolute path to tests/fixtures/rt_skin_bend.gltf>\n";return 2;}
    auto mesh=ModelLoader::LoadModel(argv[1]);
    Require(mesh.bones.size()==2,"both weighted joints must survive Assimp import");
    Require(mesh.animations.size()==1&&mesh.animations[0].channels.size()==1,"animated channel must survive import");
    const auto& keys=mesh.animations[0].channels[0].keyframes;
    Require(keys.size()==5,"fixture must retain all rotation keys");
    for(size_t i=0;i<keys.size();++i){
        Require(std::abs(keys[i].time-float(i)*.5f)<1e-5f,"rotation key array stride/time must match DLL ABI");
        Require(std::abs(glm::length(keys[i].rotation)-1.0f)<1e-5f,"rotation quaternions must remain normalized");
    }
    Require(ModelLoader::SampleAnimation(mesh.animations[0],.5f,mesh.bones),"clip sampling must succeed");
    std::vector<glm::mat4> matrices;
    for(const auto& b:mesh.bones)matrices.push_back(b.globalTransform*b.offsetMatrix);
    Require(std::abs(matrices[1][0].x-std::cos(glm::radians(55.0f)))<1e-5f,"inverse bind/rotation composition must remain correct");
    glm::vec3 lo(1e30f),hi(-1e30f);
    for(const auto& sub:mesh.subMeshes)for(const auto& v:sub.vertices){
        const auto p=mikan::rt::SkinRayTracingVertex(v,matrices).Position;
        Require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z),"deformed vertices must remain finite");
        lo=glm::min(lo,p);hi=glm::max(hi,p);
    }
    Require(std::abs(lo.x+.945339f)<1e-4f&&std::abs(hi.y-1.75379f)<1e-4f,"deformed bounds must match the known 55-degree pose");
    std::cout<<"PASS: animated glTF bone weights, inverse bind, five rotation keys, pose sampling and deformed bounds\n";
}
