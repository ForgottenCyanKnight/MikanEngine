#include "Core/CpuStageTrace.h"
#include "Rendering/VoxFaceMaterial.h"
// Little-endian, versioned surface asset. No raw C++ structs or source dependency.
#include <fstream>
#include <cstring>
#include <cmath>
#include "zlib/zlib.h"
#include "Rendering/VoxSparseDag.h"
namespace {
uint32_t SurfaceHash(const std::vector<uint32_t>& w) {
    uint32_t h=2166136261u;
    for(auto x:w)for(int b=0;b<4;++b){h^=(x>>(b*8))&255u;h*=16777619u;}
    return h;
}
uint32_t SurfaceFloat(float f){uint32_t u;std::memcpy(&u,&f,4);return u;}
float SurfaceFloat(uint32_t u){float f;std::memcpy(&f,&u,4);return f;}
std::vector<uint32_t> SurfaceCacheHeader(const std::string& source,bool uniform){
    uint64_t size=0,time=0;if(!source.empty()){auto p=std::filesystem::u8path(source);size=std::filesystem::file_size(p);time=uint64_t(std::filesystem::last_write_time(p).time_since_epoch().count());}
    return {0x5a4d5856u,1u,0x20261024u,uint32_t(uniform)|(StaticContactBackfillRequested()?2u:0u),uint32_t(size),uint32_t(size>>32),uint32_t(time),uint32_t(time>>32),0,0,0,0};
}
bool SurfaceCacheMatches(const std::filesystem::path& path,const std::string& source,bool uniform){
    try{std::ifstream in(path,std::ios::binary);uint32_t h[12]{};in.read(reinterpret_cast<char*>(h),sizeof(h));if(!in)return false;
        const auto expected=SurfaceCacheHeader(source,uniform);return std::equal(h,h+8,expected.begin());}catch(...){return false;}
}
void WriteCompressedSurface(const std::filesystem::path& path,const std::vector<uint32_t>& words,const std::string& source,bool uniform){
    if(words.size()>size_t(0x7fffffffu)/4)throw std::runtime_error("Surface cache exceeds 2 GiB limit");
    uLong raw=uLong(words.size()*4),packed=compressBound(raw);std::vector<uint8_t> data(packed);
    if(compress2(data.data(),&packed,reinterpret_cast<const Bytef*>(words.data()),raw,Z_BEST_SPEED)!=Z_OK)throw std::runtime_error("Surface compression failed");
    auto h=SurfaceCacheHeader(source,uniform);h[8]=uint32_t(raw);h[10]=uint32_t(packed);
    auto temp=path;temp+=L".tmp";std::ofstream out(temp,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(h.data()),h.size()*4);out.write(reinterpret_cast<const char*>(data.data()),packed);out.flush();if(!out)throw std::runtime_error("Cannot write compressed surface");out.close();
#ifdef _WIN32
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot publish surface cache");
#else
    std::filesystem::rename(temp,path);
#endif
}
}
void VoxRenderer::CaptureCompiledGrid(const VoxFormat::VoxData& data){
    if(data.models.empty())return;const auto& model=data.models[0];
    auto pad=[](uint32_t n){uint32_t p=1;while(p<n)p*=2;return p;};
    m_CompiledDimensions={model.sizeX,model.sizeY,model.sizeZ};
    const uint32_t tx=pad(model.sizeX),ty=pad(model.sizeY),tz=pad(model.sizeZ);
    m_CompiledCells.assign(size_t(tx)*ty*tz,0);glm::ivec3 lo(INT_MAX),hi(INT_MIN);
    for(const auto& v:model.voxels){lo=glm::min(lo,glm::ivec3(v.x,v.y,v.z));hi=glm::max(hi,glm::ivec3(v.x,v.y,v.z));m_CompiledCells[(size_t(v.z)*ty+v.y)*tx+v.x]=v.colorIndex;}
    m_CompiledPalette.resize(256);for(int i=0;i<256;++i){auto c=data.palette[i];m_CompiledPalette[i]=glm::vec4(c.r/255.f,c.g/255.f,c.b/255.f,glm::uintBitsToFloat(PackVoxFaceMaterial(data,i)));}
    glm::vec3 center=glm::vec3(lo+hi+glm::ivec3(1))*.5f;m_CompiledGridToLocal=glm::mat4(1);
    m_CompiledGridToLocal[1]=glm::vec4(0,0,1,0);m_CompiledGridToLocal[2]=glm::vec4(0,1,0,0);m_CompiledGridToLocal[3]=glm::vec4(-center.x,-center.z,-center.y,1);
}
void VoxRenderer::RestoreCompiledGrid(){
    const std::string name=m_DdaName.empty()?m_FilePath:m_DdaName;
    if(!mikan::render::VoxelDDARegistry::BootEnabled()||name.empty()||m_CompiledCells.empty())return;
    auto pad=[](uint32_t n){uint32_t p=1;while(p<n)p*=2;return p;};
    if(!m_Texture3DManagerInitialized){VoxelTexture3DManagerConfig config;config.maxTextures=64;config.format=VK_FORMAT_R8_UINT;config.enableMipmaps=true;config.filter=VK_FILTER_NEAREST;m_Texture3DManagerInitialized=m_Texture3DManager.Initialize(config);}
    if(!m_Texture3DManagerInitialized)throw std::runtime_error("Cannot initialize cached grid textures");
    auto d=m_CompiledDimensions;auto texture=m_Texture3DManager.CreateTexture3D(name+":hwrt-dda",m_CompiledCells,pad(d.x),pad(d.y),pad(d.z));
    const auto* image=m_Texture3DManager.GetTexture3D(texture);if(!image)throw std::runtime_error("Cannot upload cached grid");
    auto transform=m_CompiledGridToLocal;for(int c=0;c<4;++c)for(int r=0;r<3;++r)transform[c][r]*=m_VoxelSize;
    m_DdaGridId=mikan::render::VoxelDDARegistry::Get().Register(name,(void*)image->imageView,d.x,d.y,d.z,m_CompiledPalette,transform,m_CompiledCells);
}
void VoxRenderer::EnsureSurfaceGeometry() const {
    if(m_DeferredSurface.empty())return;
    auto* self=const_cast<VoxRenderer*>(this);
    auto words=std::move(self->m_DeferredSurface);self->m_DeferredSurface.clear();
    if(!self->LoadCompiledSurfaceWords(std::move(words),self->m_VoxelSize))throw std::runtime_error("Cannot restore deferred VOX surface");
}
std::vector<uint32_t> VoxRenderer::EncodeCompiledSurface() const {
    EnsureSurfaceGeometry();
    if(IsComposite()){
        std::vector<uint32_t> w{0x534d5856u,3u,m_UniformSurfaceMaterials?1u:0u,uint32_t(m_Submeshes.size()),uint32_t(m_SurfaceInstances.size())};
        for(const auto& child:m_Submeshes){auto words=child->EncodeCompiledSurface();w.push_back(uint32_t(words.size()));w.insert(w.end(),words.begin(),words.end());}
        for(const auto& instance:m_SurfaceInstances){w.push_back(instance.modelIndex);
            for(int col=0;col<4;++col)for(int row=0;row<3;++row)w.push_back(SurfaceFloat(instance.transform[col][row]/(col==3?m_VoxelSize:1.f)));
        }
        w.push_back(SurfaceHash(w));return w;
    }
    std::vector<uint32_t> w{0x534d5856u,1u,m_UniformSurfaceMaterials?1u:0u,uint32_t(m_VoxelCount),uint32_t(m_Faces.size()),uint32_t(m_SurfaceAttributes.size())};
    for(int a=0;a<3;++a)w.push_back(SurfaceFloat(m_MinBounds[a]/m_VoxelSize));
    for(int a=0;a<3;++a)w.push_back(SurfaceFloat(m_MaxBounds[a]/m_VoxelSize));
    for(int d=0;d<6;++d){uint32_t count=0;for(auto& f:m_Faces)count+=(f.data&255u)==uint32_t(d);w.push_back(count);}
    for(size_t i=0;i<m_Faces.size();++i){
        const auto& f=m_Faces[i];const int d=f.data&255u;
        const auto extent=f.size/m_VoxelSize;
        glm::vec3 p=(f.position-m_MinBounds)/m_VoxelSize;
        p-=d<2?glm::vec3(extent*.5f,0):(d<4?glm::vec3(0,extent.y*.5f,extent.x*.5f):glm::vec3(extent.x*.5f,0,extent.y*.5f));
        if(d==0)--p.z;else if(d==3)--p.x;else if(d==4)--p.y;
        auto o=glm::ivec3(glm::round(p));
        w.push_back(uint32_t(o.x)|(uint32_t(o.y)<<8)|(uint32_t(o.z)<<16)|(uint32_t(std::lround(extent.x)-1)<<24));
        w.push_back(uint32_t(std::lround(extent.y)-1)|(f.data&0xffffff00u));
        w.push_back(m_FaceMaterials[i]);
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
    // V5 leaf: v2 plane geometry, one tagged word per attribute descriptor.
    w.resize(18);w[1]=5;w.push_back(uint32_t(planes.size()));
    for(auto plane:planes){w.push_back(plane.firstQuad);w.push_back(plane.planeDirection);}
    w.insert(w.end(),packed.begin(),packed.end());
    w.insert(w.end(),m_SurfaceAttributes.begin(),m_SurfaceAttributes.end());w.push_back(SurfaceHash(w));
    if(!m_CompiledCells.empty()){
        std::vector<uint32_t> e{0x534d5856u,6u,m_UniformSurfaceMaterials?1u:0u,uint32_t(w.size()),m_CompiledDimensions.x,m_CompiledDimensions.y,m_CompiledDimensions.z,uint32_t(m_CompiledCells.size())};
        e.insert(e.end(),w.begin(),w.end());
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)e.push_back(SurfaceFloat(m_CompiledGridToLocal[c][r]));
        for(const auto& p:m_CompiledPalette)for(int c=0;c<4;++c)e.push_back(SurfaceFloat(p[c]));
        const auto pad=[](uint32_t n){uint32_t p=1;while(p<n)p*=2;return p;};
        const char* dagOption=std::getenv("MIKAN_VOXMESH_SVDAG");
        if(!dagOption||dagOption[0]!='0'){
            auto dag=mikan::vox::EncodeSparseDag(m_CompiledCells,pad(m_CompiledDimensions.x),pad(m_CompiledDimensions.y),pad(m_CompiledDimensions.z));
            if(dag.size()*4<m_CompiledCells.size()){
                e[1]=7u;e.insert(e.end(),dag.begin(),dag.end());
            }
        }
        if(e[1]==6u){size_t base=e.size();e.resize(base+(m_CompiledCells.size()+3)/4,0);std::memcpy(e.data()+base,m_CompiledCells.data(),m_CompiledCells.size());}
        e.push_back(SurfaceHash(e));return e;
    }
    return w;
}
namespace {
VoxFormat::VoxData ComponentData(const VoxFormat::VoxData& source,uint32_t index) {
    if(index>=source.models.size())throw std::runtime_error("Bad VOX component reference");
    VoxFormat::VoxData leaf;
    std::copy(std::begin(source.palette),std::end(source.palette),std::begin(leaf.palette));
    std::copy(std::begin(source.materials),std::end(source.materials),std::begin(leaf.materials));
    leaf.hasCustomPalette=source.hasCustomPalette;leaf.hasMaterials=source.hasMaterials;
    leaf.models.push_back(source.models[index]);return leaf;
}
}
bool VoxRenderer::MeasureSurfaceFile(const std::string& input,VoxSurfaceReductionStats& stats,bool uniform) {
    stats={};
    try {
        VoxFormat::VoxData data;
        if(!VoxFormat::LoadVoxFile(input,data)||data.models.empty())return false;
        std::map<uint32_t,uint64_t> placements;
        if(data.instances.empty())placements[0]=1;
        else for(const auto& instance:data.instances)++placements[instance.modelIndex];
        VoxSurfaceReductionStats result;
        for(const auto& [index,count]:placements){
            if(index>=data.models.size())throw std::runtime_error("Invalid VOX component index");
            VoxRenderer renderer;renderer.m_UniformSurfaceMaterials=uniform;renderer.m_VoxelSize=1;
            renderer.BuildVoxelFaces(data.instances.empty()?data:ComponentData(data,index),1);
            const auto& leaf=renderer.GetSurfaceReductionStats();
            result.exposedFaceCount+=leaf.exposedFaceCount*count;
            result.greedyQuadCount+=leaf.greedyQuadCount*count;
            result.finalQuadCount+=leaf.finalQuadCount*count;
        }
        stats=result;return true;
    }catch(const std::exception& e){LOGSTREAM(Error)<<"[VOX reduction] "<<e.what()<<std::endl;return false;}
}
bool VoxRenderer::CookSurfaceFile(const std::string& input,const std::string& output,bool uniform) {
    try {
        VoxFormat::VoxData data;if(!VoxFormat::LoadVoxFile(input,data))return false;
        std::vector<uint32_t> w;
        if(data.instances.empty()){
            VoxRenderer renderer;renderer.m_UniformSurfaceMaterials=uniform;renderer.m_VoxelSize=1;
            renderer.BuildVoxelFaces(data,1);if(renderer.m_Faces.empty())return false;
            renderer.CaptureCompiledGrid(data);
            w=renderer.EncodeCompiledSurface();
        }else{
            std::map<uint32_t,uint32_t> remap;
            for(const auto& instance:data.instances)if(!remap.contains(instance.modelIndex))
                remap.emplace(instance.modelIndex,uint32_t(remap.size()));
            // V3 container: magic/version/flags/meshCount/instanceCount,
            // followed by length-prefixed complete v5 meshes and affine instances.
            w={0x534d5856u,3u,uniform?1u:0u,uint32_t(remap.size()),uint32_t(data.instances.size())};
            std::vector<uint32_t> order(remap.size());for(auto [source,dest]:remap)order[dest]=source;
            for(auto index:order){
                VoxRenderer renderer;renderer.m_UniformSurfaceMaterials=uniform;renderer.m_VoxelSize=1;
                renderer.BuildVoxelFaces(ComponentData(data,index),1);if(renderer.m_Faces.empty())return false;
                renderer.CaptureCompiledGrid(ComponentData(data,index));
                auto mesh=renderer.EncodeCompiledSurface();w.push_back(uint32_t(mesh.size()));w.insert(w.end(),mesh.begin(),mesh.end());
            }
            for(const auto& instance:data.instances){
                w.push_back(remap.at(instance.modelIndex));
                for(int col=0;col<4;++col)for(int row=0;row<3;++row)w.push_back(SurfaceFloat(instance.transform[col][row]));
            }
            w.push_back(SurfaceHash(w));
        }
        if(StaticContactBackfillRequested()&&!uniform){
            // V4 static-contact envelope. Payload is a complete checksummed v5
            // leaf or v3 assembly. Old readers reject the outer version.
            std::vector<uint32_t> envelope{0x534d5856u,4u,2u,0x42464c02u,uint32_t(w.size())};
            envelope.insert(envelope.end(),w.begin(),w.end());envelope.push_back(SurfaceHash(envelope));
            w=std::move(envelope);
            LOGI("[VOX cook] v4 static-contact asset: fixed opaque contacts; entity emission prohibited");
        }
        WriteCompressedSurface(std::filesystem::u8path(output),w,input,uniform);return true;
    }catch(const std::exception& e){LOGSTREAM(Error)<<"[VOX cook] "<<e.what()<<std::endl;return false;}
}
bool VoxRenderer::LoadCompiledSurface(const std::string& path,float voxelSize) {
    Core::CpuStageTrace cpuStageTrace("vox.compiled_load");
    ClearComposite();m_Loaded=false;
    try {
        std::ifstream in(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);
        const auto size=in.tellg();if(size<24||size>0x7fffffff)throw std::runtime_error("Invalid size");
        in.seekg(0);std::vector<uint32_t>w;
        uint32_t magic=0;in.read(reinterpret_cast<char*>(&magic),4);in.seekg(0);
        if(magic==0x5a4d5856u){
            uint32_t h[12]{};in.read(reinterpret_cast<char*>(h),sizeof(h));
            if(!in||h[1]!=1||(h[2]!=0x20261023u&&h[2]!=0x20261024u)||h[9]||h[11]||h[8]<24||h[8]%4||h[8]>0x7fffffffu||uint64_t(h[10])+sizeof(h)!=uint64_t(size))throw std::runtime_error("Invalid compressed surface header");
            std::vector<uint8_t> packed(h[10]);in.read(reinterpret_cast<char*>(packed.data()),packed.size());if(!in)throw std::runtime_error("Truncated compressed surface");
            w.resize(h[8]/4);uLongf raw=h[8];if(uncompress(reinterpret_cast<Bytef*>(w.data()),&raw,packed.data(),uLong(packed.size()))!=Z_OK||raw!=h[8])throw std::runtime_error("Surface decompression/checksum failed");
            const bool loaded=LoadCompiledSurfaceWords(std::move(w),voxelSize);if(loaded)LOGI("[VOX disk] loaded %s; zlib=%u raw=%u bytes; preprocessing skipped",path.c_str(),h[10],h[8]);return loaded;
        }
        if(size%4!=0)throw std::runtime_error("Invalid legacy size");w.resize(size_t(size)/4);
        for(auto& x:w){x=0;for(int b=0;b<4;++b){int c=in.get();if(c<0)throw std::runtime_error("Truncated asset");x|=uint32_t(c)<<(b*8);}}
        return LoadCompiledSurfaceWords(std::move(w),voxelSize);
    }catch(const std::exception& e){LOGSTREAM(Error)<<"[VOX compiled surface] "<<path<<": "<<e.what()<<std::endl;return false;}
}
bool VoxRenderer::LoadCompiledSurfaceWords(std::vector<uint32_t> w,float voxelSize) {
    try {
        if(!(voxelSize>0)||!std::isfinite(voxelSize))throw std::runtime_error("Bad voxel size");
        if(w.size()<6)throw std::runtime_error("Truncated surface header");
        if(w[0]==0x534d5856u&&(w[1]==6u||w[1]==7u)){
            const auto hash=w.back();w.pop_back();if(SurfaceHash(w)!=hash||w.size()<8)throw std::runtime_error("Bad voxel surface checksum");
            const size_t n=w[3],bytes=w[7],base=8+n;
            if(!n||base>w.size()||(w[1]==6u?w.size()-base!=1040+(bytes+3)/4:w.size()-base<1047))throw std::runtime_error("Bad voxel surface lengths");
            glm::uvec3 dimensions(w[4],w[5],w[6]);auto pad=[](uint32_t n){uint32_t p=1;while(p<n&&p<=256)p*=2;return p;};
            if(!dimensions.x||!dimensions.y||!dimensions.z||dimensions.x>256||dimensions.y>256||dimensions.z>256||size_t(pad(dimensions.x))*pad(dimensions.y)*pad(dimensions.z)!=bytes)throw std::runtime_error("Bad voxel grid dimensions");
            std::vector<uint32_t> surface(w.begin()+8,w.begin()+base);
            if(mikan::render::VoxelDDARegistry::BootEnabled()){
                if(surface.size()<19||surface[0]!=0x534d5856u||surface[1]!=5u||surface[2]>1u)throw std::runtime_error("Invalid deferred surface");
                m_VoxelSize=voxelSize;m_VoxelCount=surface[3];m_UniformSurfaceMaterials=surface[2]!=0;
                for(int a=0;a<3;++a){m_MinBounds[a]=SurfaceFloat(surface[6+a])*voxelSize;m_MaxBounds[a]=SurfaceFloat(surface[9+a])*voxelSize;}
                m_DeferredSurface=std::move(surface);m_Loaded=true;m_useCachedMesh=true;++m_GeometryRevision;
            }else if(!LoadCompiledSurfaceWords(std::move(surface),voxelSize))return false;
            m_CompiledDimensions=dimensions;
            for(int c=0;c<4;++c)for(int r=0;r<4;++r){float value=SurfaceFloat(w[base+c*4+r]);if(!std::isfinite(value))throw std::runtime_error("Invalid grid transform");m_CompiledGridToLocal[c][r]=value;}
            m_CompiledPalette.resize(256);for(int i=0;i<256;++i)for(int c=0;c<4;++c)m_CompiledPalette[i][c]=SurfaceFloat(w[base+16+i*4+c]);
            if(w[1]==7u)m_CompiledCells=mikan::vox::DecodeSparseDag(std::span<const uint32_t>(w).subspan(base+1040),pad(dimensions.x),pad(dimensions.y),pad(dimensions.z));
            else {m_CompiledCells.resize(bytes);std::memcpy(m_CompiledCells.data(),w.data()+base+1040,bytes);}
            m_HasEmissiveQuads=false;for(const auto& p:m_CompiledPalette)if(glm::floatBitsToUint(p.w)&2u)m_HasEmissiveQuads=true;
            RestoreCompiledGrid();
            if(m_DdaGridId==UINT32_MAX)EnsureSurfaceGeometry();
            return true;
        }
        const auto hash=w.back();w.pop_back();if(SurfaceHash(w)!=hash||w[0]!=0x534d5856u||(w[1]!=1&&w[1]!=2&&w[1]!=3&&w[1]!=4&&w[1]!=5)||(w[1]!=4&&w[2]>1))throw std::runtime_error("Bad header/checksum");
        if(w[1]==4){
            if(w.size()<11||w[2]!=2u||w[3]!=0x42464c02u||size_t(w[4])!=w.size()-5)
                throw std::runtime_error("Invalid static-contact metadata/version");
            if(m_UniformSurfaceMaterials||w[7]!=0u)
                throw std::runtime_error("Static contact surface cannot be used for entity emission");
            if(w[5]!=0x534d5856u||(w[6]!=1u&&w[6]!=2u&&w[6]!=3u&&w[6]!=5u&&w[6]!=6u&&w[6]!=7u))
                throw std::runtime_error("Invalid static-contact payload");
            std::vector<uint32_t> payload(w.begin()+5,w.end());
            if(!LoadCompiledSurfaceWords(std::move(payload),voxelSize))return false;
            LOGI("[VOX compiled static contact] v4 algorithm=0x42464c02 fixed contacts, no entity emission, greedy passes=0");
            return true;
        }
        if(w[1]==3){
            const size_t meshes=w[3],instances=w[4];
            if(!meshes||meshes>65536||!instances||instances>1000000)throw std::runtime_error("Bad assembly counts");
            if(m_UniformSurfaceMaterials&&!w[2])throw std::runtime_error("Cook assembly with --uniform-materials for entity emission");
            size_t offset=5;
            for(size_t i=0;i<meshes;++i){
                if(offset>=w.size())throw std::runtime_error("Truncated component");
                const size_t count=w[offset++];
                if(count<19||count>w.size()-offset)throw std::runtime_error("Bad component length");
                std::vector<uint32_t> mesh(w.begin()+offset,w.begin()+offset+count);offset+=count;
                if(mesh[0]!=0x534d5856u||(mesh[1]!=1&&mesh[1]!=2&&mesh[1]!=5&&mesh[1]!=6&&mesh[1]!=7)||mesh[2]!=w[2])throw std::runtime_error("Bad component header");
                auto child=std::make_unique<VoxRenderer>();child->m_UniformSurfaceMaterials=m_UniformSurfaceMaterials;child->m_CompositePreview=IsPreviewPipeline();
                child->m_DdaName=(m_FilePath.empty()?"<compiled>":m_FilePath)+"#comp"+std::to_string(i);
                m_Submeshes.push_back(std::move(child));auto& leaf=*m_Submeshes.back();
                if(m_CompositeRenderPass)leaf.Init(m_CompositeRenderPass);
                if(!leaf.LoadCompiledSurfaceWords(std::move(mesh),voxelSize))throw std::runtime_error("Invalid component");
            }
            if(instances>(w.size()-offset)/13||offset+instances*13!=w.size())throw std::runtime_error("Bad instance table");
            for(size_t i=0;i<instances;++i){
                VoxFormat::SceneInstance instance;instance.modelIndex=w[offset++];
                if(instance.modelIndex>=meshes)throw std::runtime_error("Bad instance mesh index");
                for(int col=0;col<4;++col)for(int row=0;row<3;++row){float value=SurfaceFloat(w[offset++]);
                    if(!std::isfinite(value))throw std::runtime_error("Nonfinite component transform");
                    instance.transform[col][row]=value*(col==3?voxelSize:1.f);
                }
                // VOX node rotations are signed permutations, never arbitrary scale/shear.
                const auto rotation=glm::mat3(instance.transform);
                for(int row=0;row<3;++row){int nonzero=0;for(int col=0;col<3;++col){const auto v=rotation[col][row];
                    if(v!=0.f&&v!=1.f&&v!=-1.f)throw std::runtime_error("Invalid VOX rotation");nonzero+=v!=0.f;}
                    if(nonzero!=1)throw std::runtime_error("Invalid VOX rotation row");}
                if(std::abs(glm::determinant(rotation))!=1.f)throw std::runtime_error("Singular VOX rotation");
                m_SurfaceInstances.push_back(instance);
            }
            FinishComposite(voxelSize);m_UniformSurfaceMaterials=w[2]!=0;
            LOGI("[VOX compiled assembly] meshes=%zu instances=%zu greedy passes=0",meshes,instances);return true;
        }
        if(w.size()<18)throw std::runtime_error("Truncated leaf header");
        const size_t n=w[4],a=w[5];
        const bool compactDescriptors=w[1]==5;
        if(w[1]==2||compactDescriptors){
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
        if(attrs[0]!=4||attrs[1]<4||attrs[1]>516||(attrs[1]-4)%2||attrs[2]!=attrs[1]+(compactDescriptors?1:2)*n||attrs[2]>a||attrs[3]!=0)throw std::runtime_error("Bad palette layout");
        const uint32_t palette=(attrs[1]-4)/2;size_t total=0;std::vector<VoxelFaceData> faces;std::vector<uint32_t> materials;
        for(int d=0;d<6;++d)for(uint32_t j=0;j<w[12+d];++j){
            if(total>=n)throw std::runtime_error("Bad direction counts");const size_t i=total++;const uint32_t g=w[18+i*3],c=w[19+i*3];
            const uint32_t width=(g>>24)+1,height=(c&255)+1;
            const auto descriptor=attrs[attrs[1]+i*(compactDescriptors?1:2)];
            const uint32_t value=compactDescriptors?(descriptor&0x7fffffffu):descriptor;
            const uint32_t extent=compactDescriptors?((descriptor&0x80000000u)?width|(height<<16):0u):attrs[attrs[1]+i*2+1];
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
        if(!compactDescriptors){
            // Old assets remain readable; normalize once on load, never in a shader.
            std::vector<uint32_t> compact(attrs.begin(),attrs.begin()+attrs[1]);
            compact[2]=attrs[1]+uint32_t(n);
            for(size_t i=0;i<n;++i){const uint32_t value=attrs[attrs[1]+i*2],extent=attrs[attrs[1]+i*2+1];
                if(value>=0x80000000u)throw std::runtime_error("Legacy R8 offset exceeds descriptor");
                compact.push_back(value|(extent?0x80000000u:0u));}
            compact.insert(compact.end(),attrs.begin()+attrs[2],attrs.end());attrs=std::move(compact);
        }
        m_VoxelSize=voxelSize;m_MinBounds=lo*voxelSize;m_MaxBounds=hi*voxelSize;m_VoxelCount=w[3];m_UniformSurfaceMaterials=w[2]!=0;
        m_Faces=std::move(faces);m_FaceMaterials=std::move(materials);m_SurfaceAttributes=std::move(attrs);m_VoxelGrid.clear();m_useCachedMesh=true;
        BuildTriangleMesh();m_Loaded=true;if(m_DdaName.empty()||VoxLoadVerbose())LOGI("[VOX compiled surface] quads=%zu bytes=%lld greedy passes=0",n,static_cast<long long>((w.size()+1)*4));return true;
    }catch(const std::exception& e){ClearComposite();m_Loaded=false;LOGSTREAM(Error)<<"[VOX surface] "<<e.what()<<std::endl;return false;}
}
