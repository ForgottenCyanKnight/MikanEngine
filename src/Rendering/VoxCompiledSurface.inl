// Little-endian, versioned surface asset. No raw C++ structs or source dependency.
#include <fstream>
#include <cstring>
#include <cmath>
namespace {
uint32_t SurfaceHash(const std::vector<uint32_t>& w) {
    uint32_t h=2166136261u;
    for(auto x:w)for(int b=0;b<4;++b){h^=(x>>(b*8))&255u;h*=16777619u;}
    return h;
}
uint32_t SurfaceFloat(float f){uint32_t u;std::memcpy(&u,&f,4);return u;}
float SurfaceFloat(uint32_t u){float f;std::memcpy(&f,&u,4);return f;}
}
bool VoxRenderer::CookSurfaceFile(const std::string& input,const std::string& output,bool uniform) {
    VoxFormat::VoxData data;if(!VoxFormat::LoadVoxFile(input,data))return false;
    VoxRenderer renderer;renderer.m_UniformSurfaceMaterials=uniform;renderer.m_VoxelSize=1;
    renderer.BuildVoxelFaces(data,1);
    if(renderer.m_Faces.empty())return false;
    std::vector<uint32_t> w{0x534d5856u,1u,uniform?1u:0u,uint32_t(renderer.m_VoxelCount),uint32_t(renderer.m_Faces.size()),uint32_t(renderer.m_SurfaceAttributes.size())};
    for(int a=0;a<3;++a)w.push_back(SurfaceFloat(renderer.m_MinBounds[a]));
    for(int a=0;a<3;++a)w.push_back(SurfaceFloat(renderer.m_MaxBounds[a]));
    for(int d=0;d<6;++d){uint32_t count=0;for(auto& f:renderer.m_Faces)count+=(f.data&255u)==uint32_t(d);w.push_back(count);}
    for(size_t i=0;i<renderer.m_Faces.size();++i){
        const auto& f=renderer.m_Faces[i];const int d=f.data&255u;
        glm::vec3 p=f.position-renderer.m_MinBounds;
        p-=d<2?glm::vec3(f.size*.5f,0):(d<4?glm::vec3(0,f.size.y*.5f,f.size.x*.5f):glm::vec3(f.size.x*.5f,0,f.size.y*.5f));
        if(d==0)--p.z;else if(d==3)--p.x;else if(d==4)--p.y;
        auto o=glm::ivec3(glm::round(p));
        w.push_back(uint32_t(o.x)|(uint32_t(o.y)<<8)|(uint32_t(o.z)<<16)|(uint32_t(std::lround(f.size.x)-1)<<24));
        w.push_back(uint32_t(std::lround(f.size.y)-1)|(f.data&0xffffff00u));
        w.push_back(renderer.m_FaceMaterials[i]);
    }
    std::vector<VoxPlaneRange> planes;std::vector<uint32_t> packed;
    size_t index=0;
    for(uint32_t d=0;d<6;++d)for(uint32_t j=0;j<w[12+d];++j){
        const uint32_t g=w[18+index*3],a=w[19+index*3];
        uint32_t xyz[3]{g&255u,(g>>8)&255u,(g>>16)&255u};
        const int axis=d<2?2:(d<4?0:1),u=d<2?0:(d<4?2:0),v=d<4?1:2;
        AppendVoxPlane(planes,uint32_t(index),xyz[axis],d);
        packed.push_back(xyz[u]|(xyz[v]<<8)|((g>>24)<<16)|((a&255u)<<24));++index;
    }
    w.resize(18);w[1]=2;w.push_back(uint32_t(planes.size()));
    for(auto plane:planes){w.push_back(plane.firstQuad);w.push_back(plane.planeDirection);}
    w.insert(w.end(),packed.begin(),packed.end());
    w.insert(w.end(),renderer.m_SurfaceAttributes.begin(),renderer.m_SurfaceAttributes.end());w.push_back(SurfaceHash(w));
    std::ofstream out(std::filesystem::u8path(output),std::ios::binary|std::ios::trunc);
    for(auto x:w)for(int b=0;b<4;++b)out.put(char(x>>(b*8)));
    out.flush();return bool(out);
}
bool VoxRenderer::LoadCompiledSurface(const std::string& path,float voxelSize) {
    try {
        std::ifstream in(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);
        const auto size=in.tellg();if(size<76||size>512*1024*1024||size%4!=0)throw std::runtime_error("Invalid size");
        in.seekg(0);std::vector<uint32_t>w(size_t(size)/4);
        for(auto& x:w){x=0;for(int b=0;b<4;++b){int c=in.get();if(c<0)throw std::runtime_error("Truncated asset");x|=uint32_t(c)<<(b*8);}}
        const auto hash=w.back();w.pop_back();if(SurfaceHash(w)!=hash||w[0]!=0x534d5856u||(w[1]!=1&&w[1]!=2)||w[2]>1)throw std::runtime_error("Bad header/checksum");
        const size_t n=w[4],a=w[5];
        if(w[1]==2){
            if(w.size()<19||!n||a<4)throw std::runtime_error("Bad v2 counts");
            const size_t planes=w[18],base=19+planes*2;
            if(!planes||planes>n||base+n+a!=w.size())throw std::runtime_error("Bad plane table size");
            std::vector<uint32_t> legacy(w.begin(),w.begin()+18);legacy[1]=1;
            size_t total=0;uint32_t directionCounts[6]{};
            for(size_t p=0;p<planes;++p){
                const auto first=w[19+p*2],next=p+1<planes?w[19+(p+1)*2]:uint32_t(n),key=w[20+p*2],direction=key>>16;
                if(next<=first||next>n)throw std::runtime_error("Bad plane end");const uint32_t count=next-first;
                if(first!=total||!count||total+count>n||direction>=6||(key&65535u)>255u)throw std::runtime_error("Bad plane run");
                for(uint32_t j=0;j<count;++j){auto decoded=DecodeVoxQuad(w[base+total],key);legacy.push_back(decoded.first);legacy.push_back(decoded.second);legacy.push_back(0);++total;}
                directionCounts[direction]+=count;
                if(p&&direction<(w[20+(p-1)*2]>>16))throw std::runtime_error("Unsorted plane directions");
            }
            if(total!=n)throw std::runtime_error("Missing plane quads");
            for(int d=0;d<6;++d)if(directionCounts[d]!=w[12+d])throw std::runtime_error("Plane direction mismatch");
            legacy.insert(legacy.end(),w.begin()+base+n,w.end());w=std::move(legacy);
        }
        if(!n||a<4||18+n*3+a!=w.size())throw std::runtime_error("Bad counts");
        if(!(voxelSize>0)||!std::isfinite(voxelSize))throw std::runtime_error("Bad voxel size");
        if(m_UniformSurfaceMaterials&&!w[2])throw std::runtime_error("Cook this asset with --uniform-materials for entity emission");
        glm::vec3 lo,hi;for(int c=0;c<3;++c){lo[c]=SurfaceFloat(w[6+c]);hi[c]=SurfaceFloat(w[9+c]);if(!std::isfinite(lo[c])||!std::isfinite(hi[c])||hi[c]<=lo[c]||hi[c]-lo[c]>256.001f)throw std::runtime_error("Bad bounds");}
        std::vector<uint32_t> attrs(w.begin()+18+n*3,w.end());
        if(attrs[0]!=4||attrs[1]<4||attrs[1]>516||(attrs[1]-4)%2||attrs[2]!=attrs[1]+2*n||attrs[2]>a||attrs[3]!=0)throw std::runtime_error("Bad palette layout");
        const uint32_t palette=(attrs[1]-4)/2;size_t total=0;std::vector<VoxelFaceData> faces;std::vector<uint32_t> materials;
        for(int d=0;d<6;++d)for(uint32_t j=0;j<w[12+d];++j){
            if(total>=n)throw std::runtime_error("Bad direction counts");const size_t i=total++;const uint32_t g=w[18+i*3],c=w[19+i*3];
            const uint32_t width=(g>>24)+1,height=(c&255)+1;const auto value=attrs[attrs[1]+i*2],extent=attrs[attrs[1]+i*2+1];
            if(!extent){if(value>=palette)throw std::runtime_error("Bad palette index");}
            else {if(extent!=(width|(height<<16))||uint64_t(value)+width*height>uint64_t(a-attrs[2])*4)throw std::runtime_error("Bad R8 block");
                for(size_t k=0;k<width*height;++k){size_t b=value+k;if(((attrs[attrs[2]+b/4]>>((b%4)*8))&255u)>=palette)throw std::runtime_error("Bad R8 index");}}
            glm::vec3 p(float(g&255),float((g>>8)&255),float((g>>16)&255));
            glm::vec3 end=p+(d<2?glm::vec3(width,height,1):(d<4?glm::vec3(1,height,width):glm::vec3(width,1,height)));
            for(int axis=0;axis<3;++axis)if(end[axis]>hi[axis]-lo[axis]+.001f)throw std::runtime_error("Quad outside bounds");
            if(d==0)++p.z;else if(d==3)++p.x;else if(d==4)++p.y;
            glm::vec2 e(width,height);p+=d<2?glm::vec3(e*.5f,0):(d<4?glm::vec3(0,e.y*.5f,e.x*.5f):glm::vec3(e.x*.5f,0,e.y*.5f));
            uint32_t representative=value;if(extent)representative=(attrs[attrs[2]+value/4]>>((value%4)*8))&255u;
            faces.push_back({(lo+p)*voxelSize,uint32_t(d),e*voxelSize});materials.push_back(attrs[attrs[0]+representative*2+1]);
        }
        if(total!=n)throw std::runtime_error("Bad direction total");
        m_VoxelSize=voxelSize;m_MinBounds=lo*voxelSize;m_MaxBounds=hi*voxelSize;m_VoxelCount=w[3];m_UniformSurfaceMaterials=w[2]!=0;
        m_Faces=std::move(faces);m_FaceMaterials=std::move(materials);m_SurfaceAttributes=std::move(attrs);m_VoxelGrid.clear();m_useCachedMesh=true;
        BuildTriangleMesh();m_Loaded=true;LOGI("[VOX compiled surface] quads=%zu bytes=%lld greedy passes=0",n,static_cast<long long>(size));return true;
    }catch(const std::exception& e){LOGSTREAM(Error)<<"[VOX compiled surface] "<<path<<": "<<e.what()<<std::endl;return false;}
}
