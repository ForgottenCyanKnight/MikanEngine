#include "Rendering/VoxSurfaceReductionStats.h"
#include <cassert>
#include <cmath>
#include <iostream>

static void Near(double a,double b){assert(std::abs(a-b)<1e-12);}
int main(){
    const VoxSurfaceReductionStats empty;
    Near(empty.ReductionFraction(),0);Near(empty.MergeFactor(),0);
    // A single cube has six exposed faces and cannot merge them together.
    const VoxSurfaceReductionStats cube{6,6,6};
    Near(cube.ReductionFraction(),0);assert(cube.TriangleCount()==12);
    // A 2x2x2 solid cube has 24 exposed unit faces, merging into six quads.
    const VoxSurfaceReductionStats solid{24,6,6};
    Near(solid.ReductionFraction(),.75);Near(solid.MergeFactor(),4);
    const VoxSurfaceReductionStats contact{24,6,4};
    Near(contact.GreedyReductionFraction(),.75);Near(contact.ReductionFraction(),5.0/6);
    // Two placements of a shared source count twice. Do not average ratios.
    VoxSurfaceReductionStats assembly;assembly+=solid;assembly+=solid;assembly+=cube;
    assert(assembly.exposedFaceCount==54&&assembly.finalQuadCount==18);
    Near(assembly.ReductionFraction(),2.0/3);Near(assembly.MergeFactor(),3);
    const VoxSurfaceReductionStats large{uint64_t(1)<<34,uint64_t(1)<<32,uint64_t(1)<<32};
    Near(large.ReductionFraction(),.75);assert(large.TriangleCount()==uint64_t(1)<<33);
    std::cout<<"VOX surface reduction stats: PASS\n";
}
