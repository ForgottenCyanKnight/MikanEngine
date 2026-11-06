#include "Rendering/RenderStats.h"
#include <cstdio>
#include <cstdlib>
static void Check(bool condition,const char* message){if(!condition){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    auto& stats=Rendering::RenderStats::Get();stats.TakeSnapshot();
    Rendering::RayTracingStats scene;scene.views=1;scene.instances=2;scene.entities=1;
    scene.trianglesMin=scene.trianglesMax=24;scene.uniqueTriangles=12;scene.geometries=1;
    stats.ReportRayTracing(0,scene);stats.ReportRayTracing(0,scene);
    auto snapshot=stats.TakeSnapshot();
    Check(snapshot.rayTracing.instances==2&&snapshot.rayTracing.trianglesMax==24,"same view must not double-count cached renders");
    Check(snapshot.rayTracing.uniqueTriangles==12,"shared BLAS triangles are separate from instanced triangles");
    Check(stats.TakeSnapshot().rayTracing.views==0,"snapshots clear RT data; switching to raster must not retain stale counters");
    Rendering::RayTracingStats game=scene;game.instances=3;game.trianglesMin=8;game.trianglesMax=36;game.gpuLodInstances=1;game.aabbs=7;
    stats.ReportRayTracing(0,scene);stats.ReportRayTracing(1,game);
    stats.OnDrawIndexed(6,2);
    snapshot=stats.TakeSnapshot();
    Check(snapshot.rayTracing.views==2&&snapshot.rayTracing.instances==5,"scene/game views aggregate independently");
    Check(snapshot.rayTracing.trianglesMin==32&&snapshot.rayTracing.trianglesMax==60,"GPU LOD ranges aggregate without presenting a false exact count");
    Check(snapshot.rayTracing.gpuLodInstances==1&&snapshot.rayTracing.aabbs==7,"AABB and GPU LOD counters remain distinct");
    Check(snapshot.drawCalls==1&&snapshot.triangles==4,"RT reports must not alter raster counters");
    stats.ReportRayTracing(1,game);stats.ReportRayTracing(1,{});
    Check(stats.TakeSnapshot().rayTracing.instances==0,"empty render replaces the previous scene");
    std::puts("RenderStats tests passed");return 0;
}
