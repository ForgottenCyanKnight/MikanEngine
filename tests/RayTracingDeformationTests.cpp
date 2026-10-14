#include "Rendering/RayTracing/RayTracingDeformation.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <cstdlib>

static void Require(bool value,const char* message){
    if(!value){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}
}
static bool Near(glm::vec3 a,glm::vec3 b,float tolerance=1e-5f){return glm::length(a-b)<tolerance;}
int main(){
    Vertex vertex{};vertex.Position={1,0,0};vertex.Normal=PackSnorm3({0,1,0});
    vertex.Tangent=PackSnorm3({1,0,0},1);vertex.TexCoords=PackHalf2({0.25f,0.75f});
    vertex.BoneIDs=glm::u8vec4(0);vertex.BoneWeights=glm::u8vec4(255,0,0,0);
    std::vector<glm::mat4> bones{glm::translate(glm::mat4(1),glm::vec3(2,3,4))};
    auto result=mikan::rt::SkinRayTracingVertex(vertex,bones);
    Require(Near(result.Position,{3,3,4}),"bone translation must deform the BLAS position");
    Require(Near(UnpackSnorm3(result.Normal),{0,1,0}),"translation must preserve normals");
    Require(result.BoneWeights==glm::u8vec4(0),"deformed vertices must not be skinned twice");
    Require(result.TexCoords==vertex.TexCoords,"skinning must preserve packed UVs");
    bones[0]=glm::rotate(glm::mat4(1),glm::radians(90.0f),glm::vec3(0,0,1));
    result=mikan::rt::SkinRayTracingVertex(vertex,bones);
    Require(Near(result.Position,{0,1,0}),"bone rotation must deform position");
    Require(Near(UnpackSnorm3(result.Normal),{-1,0,0},0.02f),"normal rotation must match geometry");
    vertex.BoneIDs=glm::u8vec4(255);bones.assign(256,glm::mat4(1));
    bones.back()=glm::translate(glm::mat4(1),glm::vec3(5,0,0));
    result=mikan::rt::SkinRayTracingVertex(vertex,bones);
    Require(Near(result.Position,{6,0,0}),"packed ID 255 must match shader MAX_BONES clamp");
    vertex.BoneIDs={0,1,0,0};vertex.BoneWeights={128,127,0,0};
    bones={glm::translate(glm::mat4(1),glm::vec3(0,2,0)),glm::translate(glm::mat4(1),glm::vec3(0,0,4))};
    result=mikan::rt::SkinRayTracingVertex(vertex,bones);
    Require(Near(result.Position,{1,256.0f/255.0f,508.0f/255.0f}),"quantized two-bone blend must preserve shader weights");
    vertex.BoneWeights=glm::u8vec4(0);
    Require(Near(mikan::rt::SkinRayTracingVertex(vertex,bones).Position,vertex.Position),"unweighted rigid vertices must retain bind positions");
    Require(Near(mikan::rt::SkinRayTracingVertex(vertex,{}).Position,vertex.Position),"empty palettes must retain bind positions");
    std::cout<<"PASS: RT skinning translation, rotation, quantized blend, ID clamp, rigid fallback, UV and double-skin prevention\n";
}
