#pragma once
#include "Platform/Export.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

// CXW packed-color world reader. Sector streaming keeps memory proportional
// to resident bricks and generated surfaces, not the world bounding volume.
namespace CxwFormat {
struct Quad {uint32_t geometry,appearance,material,palette;uint32_t direction;};
struct Mesh {
    std::vector<Quad> quads;
    std::vector<std::array<uint32_t,2>> palette;
    std::vector<uint32_t> descriptors,indexWords; // Mixed rectangles use tagged R16 indices.
    glm::mat4 transform{1}; // CXW world, rebased to the aggregate sector center.
    uint32_t voxels=0;
    uint64_t exposedFaces=0;
    uint32_t extent=256,bricks=0,sourceSectors=0;
    uint64_t hiddenFaces=0,greedyQuads=0;
};
struct Stats {
    uint16_t version=0;
    uint64_t entities=0,sectors=0,bricks=0,voxels=0,quads=0,exposedFaces=0;
    uint64_t hiddenFaces=0,greedyQuads=0;
    glm::dvec3 center{0},minimum{0},maximum{0};
    bool hasPlayerPose=false;
    glm::vec3 playerPosition{0};
    glm::vec4 playerRotation{0,0,0,1};
};
using MeshConsumer=std::function<void(Mesh&&)>;
using Progress=std::function<void(size_t,size_t)>;
// Throws on malformed/unsupported input. Glass needs Caelix's external tint
// palette, absent from the file; reject it unless MIKAN_CXW_CLEAR_GLASS=1
// explicitly opts into substituting all glass with clear-white transmission.
MIKAN_API Stats Load(const std::string& path,const MeshConsumer& consume,const Progress& progress={},bool mergeColors=true);
// CPU-only import/cook using the engine's canonical voxel surface baker.
MIKAN_API bool ConvertToVoxmesh(const std::string& input,const std::string& output,bool uniform=false);
}
