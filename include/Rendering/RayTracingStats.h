#pragma once
#include <cstdint>
namespace Rendering {
// TLAS input geometry, not ray-hit counts. GPU-selected LOD is a range until
// readback is explicitly requested; collecting these counters never stalls GPU.
struct RayTracingStats {
    uint64_t views=0,instances=0,entities=0;
    uint64_t trianglesMin=0,trianglesMax=0,uniqueTriangles=0;
    uint64_t geometries=0,aabbs=0,gpuLodInstances=0;
    void Accumulate(const RayTracingStats& s) {
        views+=s.views;instances+=s.instances;entities+=s.entities;
        trianglesMin+=s.trianglesMin;trianglesMax+=s.trianglesMax;uniqueTriangles+=s.uniqueTriangles;
        geometries+=s.geometries;aabbs+=s.aabbs;gpuLodInstances+=s.gpuLodInstances;
    }
};
}
