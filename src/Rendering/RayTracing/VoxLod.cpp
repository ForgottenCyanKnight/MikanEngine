#include "Rendering/RayTracing/VoxLod.h"
#include "Rendering/VoxRenderer.h"
#include "Rendering/VoxelDDARegistry.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectLoadProgress.h"
#include "Core/Log.h"
#include "meshoptimizer.h"
#include "zlib/zlib.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <tuple>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace mikan::rt {namespace {
int OnlyLevel(){const char* v=std::getenv("MIKAN_VOX_LOD_ONLY");return v&&v[0]?std::clamp(std::atoi(v),-1,int(kVoxLodMaxLevel)):-1;}
void RetainOnlyLevel(VoxLodSet& set){const int only=OnlyLevel();if(only<0)return;
    for(int l=0;l<3;++l)if(l+1!=only)set.levels[l]=VoxLodLevel{};}
struct Options {std::array<float,3> distances{40,80,160};int force=-1;};
const Options& Settings(){static const Options o=[](){Options x;
    if(const auto* v=std::getenv("MIKAN_VOX_LOD_FORCE"))x.force=std::clamp(std::atoi(v),-1,int(kVoxLodMaxLevel));
    if(const auto* v=std::getenv("MIKAN_VOX_LOD_DISTANCES")){std::istringstream s(v);std::array<float,3>d;
        if(s>>d[0]&&std::isfinite(d[0])&&d[0]>0)x.distances[0]=d[0];}
    return x;}();return o;}
uint64_t Hash(const void* data,size_t n,uint64_t h=1469598103934665603ull){const auto* p=static_cast<const uint8_t*>(data);for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ull;}return h;}
glm::vec3 Position(const VoxLodTriangle& t,int corner){return {t.positions[corner*3],t.positions[corner*3+1],t.positions[corner*3+2]};}
struct FaceTree {
    struct Node {glm::vec3 lo{1e30f},hi{-1e30f};uint32_t first=0,count=0,left=0,right=0;};
    const VoxRenderer& source;const std::vector<VoxelFaceData>& faces;std::vector<Node> nodes;std::vector<uint32_t> order;
    static glm::vec3 U(uint32_t d){return d<2||d>=4?glm::vec3(1,0,0):glm::vec3(0,0,1);}
    static glm::vec3 V(uint32_t d){return d<4?glm::vec3(0,1,0):glm::vec3(0,0,1);}
    explicit FaceTree(const VoxRenderer& s):source(s),faces(s.GetSurfaceFaces()) {order.resize(faces.size());std::iota(order.begin(),order.end(),0u);Build(0,uint32_t(order.size()));}
    uint32_t Build(uint32_t first,uint32_t count){const uint32_t id=uint32_t(nodes.size());nodes.emplace_back();Node node;node.first=first;node.count=count;
        for(uint32_t j=first;j<first+count;++j){const auto& f=faces[order[j]];const auto e=U(f.data&255u)*f.size.x*.5f+V(f.data&255u)*f.size.y*.5f;node.lo=glm::min(node.lo,f.position-e);node.hi=glm::max(node.hi,f.position+e);}
        if(count>8){const auto extent=node.hi-node.lo;const int axis=extent.x>=extent.y&&extent.x>=extent.z?0:extent.y>=extent.z?1:2;const uint32_t mid=first+count/2;
            std::nth_element(order.begin()+first,order.begin()+mid,order.begin()+first+count,[&](auto a,auto b){return faces[a].position[axis]<faces[b].position[axis];});
            node.left=Build(first,mid-first);node.right=Build(mid,first+count-mid);node.count=0;}
        nodes[id]=node;return id;
    }
    static float Distance(const Node& n,glm::vec3 p){const auto delta=glm::max(glm::max(n.lo-p,p-n.hi),glm::vec3(0));return glm::dot(delta,delta);}
    std::pair<uint32_t,uint32_t> Attribute(glm::vec3 p)const {float best=1e30f;uint32_t found=0;glm::vec2 uv(0);std::array<uint32_t,64> stack{};size_t top=1;
        while(top){const auto& n=nodes[stack[--top]];if(Distance(n,p)>best)continue;
            if(!n.count){const float a=Distance(nodes[n.left],p),b=Distance(nodes[n.right],p);stack[top++]=a<b?n.right:n.left;stack[top++]=a<b?n.left:n.right;continue;}
            for(uint32_t k=n.first;k<n.first+n.count;++k){const auto id=order[k];const auto& f=faces[id];const auto u=U(f.data&255u),v=V(f.data&255u),delta=p-f.position;
                const glm::vec2 q=glm::clamp(glm::vec2(glm::dot(delta,u),glm::dot(delta,v)),-f.size*.5f,f.size*.5f);
                const auto offset=p-(f.position+u*q.x+v*q.y);const float d=glm::dot(offset,offset);
                if(d<best){best=d;found=id;uv=q+f.size*.5f;}}
        }
        const auto& a=source.GetSurfaceAttributes();const auto& f=faces[found];const auto word=a.at(a.at(1)+found);uint32_t palette=word&0x3fffffffu;
        if(word&0x80000000u){const uint32_t width=uint32_t(std::lround(f.size.x/source.GetVoxelSize())),height=uint32_t(std::lround(f.size.y/source.GetVoxelSize()));
            const uint32_t x=std::min(uint32_t(std::max(0.f,uv.x/source.GetVoxelSize())),width-1),y=std::min(uint32_t(std::max(0.f,uv.y/source.GetVoxelSize())),height-1);
            const bool wide=(word&0x40000000u)!=0;const uint32_t byte=palette+(x+y*width)*(wide?2:1);palette=(a.at(a.at(2)+byte/4)>>((byte%4)*8))&(wide?65535u:255u);}
        return {a.at(a.at(0)+palette*2),a.at(a.at(0)+palette*2+1)};
    }
};
bool Load(const std::filesystem::path& path,VoxLodSet& result){try{
    std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)return false;const auto size=in.tellg();if(size<68||size>512*1024*1024)return false;in.seekg(0);uint32_t h[17]{};in.read(reinterpret_cast<char*>(h),68);
    if(!in||h[0]!=0x444f4c56u||h[1]!=2||h[2]!=uint32_t(result.key)||h[3]!=uint32_t(result.key>>32)||h[4]!=result.originalTriangles||h[5]!=result.resolution||h[7]!=kVoxLodMaxLevel||uint64_t(h[16])+68!=uint64_t(size))return false;
    uint64_t total=0;
    for(int l=0;l<3;++l){const auto n=h[8+l];if(n>4000000u||(l>=int(kVoxLodMaxLevel)&&n))return false;total+=uint64_t(n)*sizeof(VoxLodTriangle);if(!std::isfinite(std::bit_cast<float>(h[11+l]))||std::bit_cast<float>(h[11+l])<0)return false;}
    if(total!=h[14]||total>512*1024*1024)return false;
    std::vector<uint8_t> packed(h[16]),raw(std::max(size_t(total),size_t(1)),uint8_t(0));in.read(reinterpret_cast<char*>(packed.data()),packed.size());if(!in)return false;
    uLongf bytes=uLongf(raw.size());if(uncompress(raw.data(),&bytes,packed.data(),uLong(packed.size()))!=Z_OK||bytes!=total||crc32(0,raw.data(),uInt(total))!=h[15])return false;
    std::array<VoxLodLevel,3> decoded;size_t at=0;for(int l=0;l<3;++l){auto& level=decoded[l];const auto bytes=size_t(h[8+l])*sizeof(VoxLodTriangle);
        if(OnlyLevel()>=0&&OnlyLevel()!=l+1){at+=bytes;continue;}
        level.triangles.resize(h[8+l]);if(!level.triangles.empty())std::memcpy(level.triangles.data(),raw.data()+at,bytes);at+=bytes;level.error=std::bit_cast<float>(h[11+l]);
        for(const auto& t:level.triangles)for(const float x:t.positions)if(!std::isfinite(x))return false;}
    result.levels=std::move(decoded);result.remeshTriangles=h[6];result.cacheHit=true;return true;
}catch(...){for(auto& l:result.levels)l={};return false;}}
void Save(const std::filesystem::path& path,const VoxLodSet& result){try{
    uint32_t h[17]{0x444f4c56u,2u,uint32_t(result.key),uint32_t(result.key>>32),result.originalTriangles,result.resolution,result.remeshTriangles,kVoxLodMaxLevel};std::vector<uint8_t> raw;
    for(int l=0;l<3;++l){const auto& level=result.levels[l];h[8+l]=uint32_t(level.triangles.size());h[11+l]=std::bit_cast<uint32_t>(level.error);const size_t n=raw.size();raw.resize(n+level.triangles.size()*sizeof(VoxLodTriangle));if(!level.triangles.empty())std::memcpy(raw.data()+n,level.triangles.data(),raw.size()-n);}
    h[14]=uint32_t(raw.size());h[15]=crc32(0,raw.data(),uInt(raw.size()));uLongf bytes=compressBound(uLong(raw.size()));std::vector<uint8_t> packed(bytes);
    if(compress2(packed.data(),&bytes,raw.data(),uLong(raw.size()),Z_BEST_SPEED)!=Z_OK)return;h[16]=uint32_t(bytes);std::filesystem::create_directories(path.parent_path());auto temporary=path;temporary+=".tmp";
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(h),68);out.write(reinterpret_cast<const char*>(packed.data()),bytes);if(!out)return;}
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))LOGW("[VOX LOD] cache publish failed");
#else
    std::filesystem::rename(temporary,path);
#endif
}catch(...){LOGW("[VOX LOD] cache write failed; using generated geometry");}}
} // namespace
bool VoxLodEnabled(){const auto* v=std::getenv("MIKAN_VOX_LOD");if(!v||std::strcmp(v,"1")!=0)return false;
    const auto* a=std::getenv("MIKAN_VOX_HW_QUAD_AABB");const auto* s=std::getenv("MIKAN_HWRT_SOFTWARE_QUAD_BVH");
    return !mikan::render::VoxelDDARegistry::BootEnabled()&&(!a||a[0]!='1')&&(!s||s[0]!='1');}
uint32_t SelectVoxLod(float distance,uint32_t previous){const auto& o=Settings();if(o.force>=0)return uint32_t(o.force);uint32_t level=std::min(previous,kVoxLodMaxLevel);if(!std::isfinite(distance))return distance>0?kVoxLodMaxLevel:0u;
    while(level<kVoxLodMaxLevel&&distance>o.distances[level]*(previous?1.1f:1.f))++level;
    while(level&&distance<o.distances[level-1]*.9f)--level;return level;}
std::array<float,3> VoxLodDistances(){return Settings().distances;}
int VoxLodForceLevel(){return Settings().force;}
std::shared_ptr<VoxLodSet> LoadOrBuildVoxLod(const VoxRenderer& source){auto result=std::make_shared<VoxLodSet>();const auto& faces=source.GetSurfaceFaces();
    result->originalTriangles=uint32_t(faces.size()*2);if(faces.empty())return result;
    const auto& materials=source.GetSurfaceFaceMaterials();result->hasEmission=std::any_of(materials.begin(),materials.end(),[](uint32_t word){return (word&2u)!=0;});
    const uint64_t exposed=source.GetSourceExposedFaceCount();
    result->mergeFactor=double(exposed)/faces.size();result->candidate=true;
    const auto extent=(source.GetMaxBounds()-source.GetMinBounds())/source.GetVoxelSize();result->resolution=uint32_t(std::clamp(int(std::lround(std::max({extent.x,extent.y,extent.z}))),4,256));
    const char* directOption=std::getenv("MIKAN_VOX_LOD_DIRECT_REMESH_SCALE");
    const float directScale=directOption?std::strtof(directOption,nullptr):0.f;
    const bool directRemesh=std::isfinite(directScale)&&directScale>0.f&&directScale<=1.f;
    if(directRemesh)result->resolution=uint32_t(std::clamp(int(std::lround(float(result->resolution)*directScale)),4,256));
    uint64_t hash=Hash(&result->originalTriangles,4);hash=Hash(&result->resolution,4,hash);const uint32_t algorithm=0x14000001u;hash=Hash(&algorithm,4,hash);
    if(directRemesh){const uint32_t directTag=0x44495231u;hash=Hash(&directTag,4,hash);}
    for(const auto& f:faces){hash=Hash(&f.position,sizeof(f.position),hash);hash=Hash(&f.size,sizeof(f.size),hash);hash=Hash(&f.data,4,hash);}
    const auto& attrs=source.GetSurfaceAttributes();hash=Hash(attrs.data(),attrs.size()*4,hash);result->key=hash;
    std::ostringstream name;name<<std::hex<<std::setfill('0')<<std::setw(16)<<hash<<".vlod";
    auto root=std::filesystem::u8path(ProjectManager::GetInstance().GetEngineRoot());const char* custom=std::getenv("MIKAN_VOX_LOD_CACHE");const auto path=(custom?std::filesystem::u8path(custom):root/"cache/vox-lod")/name.str();
    if(Load(path,*result)){LOGI("[VOX LOD cache] hit key=%s original=%u L1=%zu L2=%zu L3=%zu",name.str().c_str(),result->originalTriangles,result->levels[0].triangles.size(),result->levels[1].triangles.size(),result->levels[2].triangles.size());return result;}
    Core::ProjectLoadProgress::Get().Update("Generating voxel remesh LOD: "+name.str());
    std::vector<glm::vec3> positions;std::vector<uint32_t> indices;positions.reserve(faces.size()*4);indices.reserve(faces.size()*6);
    const glm::vec3 normals[6]={{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    for(const auto& f:faces){const uint32_t d=f.data&255u,base=uint32_t(positions.size());const auto u=FaceTree::U(d)*f.size.x,v=FaceTree::V(d)*f.size.y,a=f.position-(u+v)*.5f;
        positions.insert(positions.end(),{a,a+u,a+u+v,a+v});if(glm::dot(glm::cross(u,v),normals[d])>0)indices.insert(indices.end(),{base,base+1,base+2,base,base+2,base+3});else indices.insert(indices.end(),{base,base+2,base+1,base,base+3,base+2});}
    const size_t bound=meshopt_remesh(nullptr,0,indices.data(),indices.size(),&positions[0].x,positions.size(),sizeof(glm::vec3),int(result->resolution),meshopt_RemeshSolve);
    std::vector<float> remeshed;
    size_t triangles=0;
    if(bound&&bound<=4000000){
        remeshed.resize(bound*9);
        triangles=meshopt_remesh(remeshed.data(),bound,indices.data(),indices.size(),&positions[0].x,positions.size(),sizeof(glm::vec3),int(result->resolution),meshopt_RemeshSolve);
        if(triangles>bound)triangles=0;
    }
    result->remeshTriangles=uint32_t(triangles);
    if(directRemesh&&triangles){
        // Diagnostic proxy: native remesher output, no simplification or welding.
        // Store in the sole proxy level for isolated inspection.
        FaceTree tree(source);auto& output=result->levels[0].triangles;output.reserve(triangles);
        for(size_t t=0;t<triangles;++t){VoxLodTriangle triangle;std::copy_n(remeshed.data()+t*9,9,triangle.positions.begin());
            const auto a=Position(triangle,0),b=Position(triangle,1),c=Position(triangle,2);const auto n=glm::cross(b-a,c-a);
            if(glm::dot(n,n)<1e-16f)continue;std::tie(triangle.appearance,triangle.material)=tree.Attribute((a+b+c)/3.f);output.push_back(triangle);}
        Save(path,*result);LOGI("[VOX direct remesh] resolution=%u scale=%.4f original=%u remesh=%u valid=%zu simplify=OFF",
            result->resolution,directScale,result->originalTriangles,result->remeshTriangles,output.size());RetainOnlyLevel(*result);return result;
    }
    // The quarter-size target is a preference, never an eligibility condition.
    // If remeshing cannot provide a useful proxy, simplify the exact source;
    // even an irreducible small mesh still receives a valid LOD1 representation.
    FaceTree tree(source);
    auto simplify=[&](const float* vertexData,size_t vertexCount,const uint32_t* sourceIndices,size_t indexCount){
        const size_t corners=indexCount;std::vector<uint32_t> remap(vertexCount);
        const size_t vertices=meshopt_generateVertexRemap(remap.data(),sourceIndices,corners,vertexData,vertexCount,12);
        std::vector<float> unique(vertices*3);std::vector<uint32_t> input(corners);
        meshopt_remapVertexBuffer(unique.data(),vertexData,vertexCount,12,remap.data());
        meshopt_remapIndexBuffer(input.data(),sourceIndices,corners,remap.data());
        std::vector<uint32_t> simplified(corners);float error=0;
        const uint32_t target=std::max(1u,result->originalTriangles/4u);
        size_t count=meshopt_simplify(simplified.data(),input.data(),input.size(),unique.data(),vertices,12,size_t(target)*3,.1f,meshopt_SimplifyPermissive|meshopt_SimplifyPreserveFolds,&error);
        if(!count||count%3||!std::isfinite(error)){simplified=input;count=simplified.size();error=0;}
        VoxLodLevel level;level.error=error*meshopt_simplifyScale(unique.data(),vertices,12);
        level.triangles.reserve(count/3);
        for(size_t t=0;t<count;t+=3){VoxLodTriangle triangle;glm::vec3 centroid(0);
            for(int k=0;k<3;++k){const auto i=simplified[t+k]*3;for(int axis=0;axis<3;++axis)triangle.positions[k*3+axis]=unique[i+axis];centroid+=Position(triangle,k)/3.f;}
            const auto n=glm::cross(Position(triangle,1)-Position(triangle,0),Position(triangle,2)-Position(triangle,0));if(glm::dot(n,n)<1e-16f)continue;
            std::tie(triangle.appearance,triangle.material)=tree.Attribute(centroid);level.triangles.push_back(triangle);
        }
        return level;
    };
    if(triangles)result->levels[0]=simplify(remeshed.data(),triangles*3,nullptr,triangles*3);
    if(result->levels[0].triangles.empty()||result->levels[0].triangles.size()>result->originalTriangles)
        result->levels[0]=simplify(&positions[0].x,positions.size(),indices.data(),indices.size());
    Save(path,*result);LOGI("[VOX LOD generated] key=%s original=%u resolution=%u remesh=%u L1=%zu merge=%.3f policy=all-vox-two-level",
        name.str().c_str(),result->originalTriangles,result->resolution,result->remeshTriangles,result->levels[0].triangles.size(),result->mergeFactor);
    RetainOnlyLevel(*result);return result;
}
} // namespace mikan::rt
