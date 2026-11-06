#pragma once
#include "Platform/Export.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
class VoxRenderer;
namespace mikan::rt {
// Temporary universal VOX policy: exact LOD0 plus one proxy, no candidate filter.
inline constexpr uint32_t kVoxLodMaxLevel=1;
struct VoxLodTriangle {std::array<float,9> positions;uint32_t appearance=0,material=0;};
static_assert(sizeof(VoxLodTriangle)==44);
struct VoxLodLevel {std::vector<VoxLodTriangle> triangles;float error=0;};
struct VoxLodSet {
    uint64_t key=0;uint32_t originalTriangles=0,resolution=0,remeshTriangles=0;
    double mergeFactor=0;bool candidate=false,cacheHit=false,hasEmission=false;
    std::array<VoxLodLevel,3> levels;
    uint32_t AvailableLevel(uint32_t requested)const {requested=std::min(requested,kVoxLodMaxLevel);while(requested&&levels[requested-1].triangles.empty())--requested;return requested;}
};
MIKAN_API bool VoxLodEnabled();
MIKAN_API std::array<float,3> VoxLodDistances();
MIKAN_API int VoxLodForceLevel();
MIKAN_API uint32_t SelectVoxLod(float distance,uint32_t previous);
MIKAN_API std::shared_ptr<VoxLodSet> LoadOrBuildVoxLod(const VoxRenderer& source);
}
