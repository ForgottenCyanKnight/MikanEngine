#include "Rendering/CxwLoader.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "RayTracing/VoxSurfaceBake.h"
#include <future>
#include <deque>
#include <thread>
#include <cstdlib>
#include "zlib/zlib.h"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace CxwFormat { namespace {
using Coord=std::array<int32_t,3>;
[[noreturn]] void Fail(const char* reason){throw std::runtime_error(std::string("CXW: ")+reason);}
struct Cursor {
    const std::vector<uint8_t>& bytes;size_t at=0;
    uint64_t Uint(size_t n){if(n>bytes.size()-at)Fail("truncated record");uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(bytes[at++])<<(i*8);return v;}
    float Float(){return std::bit_cast<float>(uint32_t(Uint(4)));}
    void Skip(size_t n){if(n>bytes.size()-at)Fail("truncated slot");at+=n;}
};
struct Region {uint64_t offset;uint32_t size;};
struct Entity {glm::dmat4 transform{1};std::map<Coord,Region> regions;};
struct Sector {
    std::array<int16_t,4096> indices{};
    std::vector<uint16_t> blocks;uint32_t bricks=0;
    uint16_t Cell(int x,int y,int z)const{
        const int id=indices[(x>>3)+16*(y>>3)+256*(z>>3)];
        return id<0?0:blocks[size_t(id)*512+(x&7)+8*(y&7)+64*(z&7)];
    }
};
struct Reader {
    std::ifstream file;uint64_t size=0;
    bool clearGlass=false;
    explicit Reader(const std::string& path):file(std::filesystem::u8path(path),std::ios::binary|std::ios::ate){
        const char* glass=std::getenv("MIKAN_CXW_CLEAR_GLASS");clearGlass=glass&&std::string(glass)=="1";
        if(!file)Fail("cannot open file");const auto end=file.tellg();if(end<64)Fail("truncated header");size=uint64_t(end);
    }
    std::vector<uint8_t> Bytes(uint64_t offset,uint64_t count){
        if(offset>size||count>size-offset||count>std::numeric_limits<size_t>::max())Fail("file offset out of range");
        std::vector<uint8_t> b(size_t(count),uint8_t(0));file.seekg(std::streamoff(offset));
        if(count)file.read(reinterpret_cast<char*>(b.data()),std::streamsize(count));if(!file)Fail("file read failed");return b;
    }
    std::shared_ptr<Sector> Decode(const Region& r){
        if(r.offset<64||r.size<16388)Fail("invalid sector region");
        auto prefix=Bytes(r.offset+16384,4);Cursor p{prefix};const uint32_t packed=uint32_t(p.Uint(4));
        if(uint64_t(packed)+16388!=r.size)Fail("sector region size mismatch");
        auto input=Bytes(r.offset+16388,packed);
        z_stream stream{};if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK)Fail("inflate initialization failed");
        struct End {z_stream& s;~End(){inflateEnd(&s);}} end{stream};
        stream.next_in=input.data();stream.avail_in=packed;
        std::vector<uint8_t> raw;
        int result=Z_OK;
        while(result!=Z_STREAM_END){
            constexpr size_t block=65536,limit=64u*1024*1024;
            if(raw.size()>=limit)Fail("sector inflate exceeds 64 MiB safety limit");
            const auto base=raw.size();raw.resize(base+block);stream.next_out=raw.data()+base;stream.avail_out=block;
            result=inflate(&stream,Z_NO_FLUSH);raw.resize(base+block-stream.avail_out);
            if(result!=Z_OK&&result!=Z_STREAM_END)Fail("invalid raw deflate payload");
            if(result==Z_OK&&!stream.avail_in&&stream.avail_out)Fail("truncated deflate payload");
        }
        if(stream.avail_in)Fail("trailing compressed bytes");
        Cursor c{raw};if(c.Uint(4)!=0x32535856u)Fail("invalid sector magic");
        const auto capacity=c.Uint(4),count=c.Uint(4),free=c.Uint(4);
        if(capacity>4096||count>capacity||free>capacity||count+free!=capacity)Fail("invalid brick counts");
        auto sector=std::make_shared<Sector>();sector->bricks=uint32_t(count);
        std::array<uint8_t,4096> used{};size_t references=0;
        for(auto& index:sector->indices){index=std::bit_cast<int16_t>(uint16_t(c.Uint(2)));
            if(index==-1)continue;if(index<0||uint32_t(index)>=capacity||used[index])Fail("invalid/duplicate brick id");used[index]=1;++references;}
        if(references!=count)Fail("allocated brick count mismatch");
        for(size_t i=0;i<free;++i){const auto id=std::bit_cast<int16_t>(uint16_t(c.Uint(2)));
            if(id<0||uint32_t(id)>=capacity||used[id])Fail("invalid freelist");used[id]=2;}
        c.Skip(2+4096*2);const auto records=c.Uint(4);if(records>16)Fail("too many slot records");
        uint32_t slots=0;
        for(size_t i=0;i<records;++i){const auto slot=c.Uint(1),stride=c.Uint(2);
            if(slot>=16||(slots&(1u<<slot))||!stride)Fail("invalid slot record");slots|=1u<<slot;
            const uint64_t bytes=capacity*512*stride;
            if(slot==0){if(stride!=2)Fail("Block stride must be 2");sector->blocks.resize(size_t(capacity)*512);
                for(auto& code:sector->blocks)code=uint16_t(c.Uint(2));
            }else {if(slot==1&&stride!=1)Fail("PhysicsInfo stride must be 1");c.Skip(size_t(bytes));}
        }
        if(!(slots&1u)||c.at!=raw.size())Fail("missing Block slot or trailing sector bytes");
        for(const auto id:sector->indices)if(id>=0)for(size_t v=0;v<512;++v){const auto code=sector->blocks[size_t(id)*512+v];
            if(code>=256&&code<0x4000)Fail("noncanonical packed voxel code");
            if(code>132&&code<256)Fail("reserved glass palette id");
            if(code&&code<256&&!clearGlass)Fail("glass requires the external Caelix glass-palette.json; use MIKAN_CXW_CLEAR_GLASS=1 for explicit clear-white substitution");}
        return sector;
    }
};
std::array<uint32_t,2> Material(uint16_t code){
    if(code&&code<256)return {0xffffff00u,8u}; // Explicit fallback: engine dielectric transmission.
    uint32_t r,g,b,word=0;
    if(code&0x8000){r=((code>>10)&31)*255/31;g=((code>>5)&31)*255/31;b=(code&31)*255/31;}
    else {r=((code>>10)&15)*17;g=((code>>6)&15)*17;b=((code>>2)&15)*17;
        word=2u|(uint32_t(glm::packHalf1x16(.1f*float(uint32_t(1)<<(3*(code&3)))))<<16);}
    return {(r<<8)|(g<<16)|(b<<24),word};
}
#include "CxwSurfaceBake.inl"
} // namespace
Stats Load(const std::string& path,const MeshConsumer& consume,const Progress& progress,bool mergeColors){
    Reader reader(path);auto header=reader.Bytes(0,64);Cursor h{header};const auto magic=h.Uint(4);Stats stats;
    if(magic!=0x534c5843u&&magic!=0x534c5856u)Fail("invalid file magic");
    stats.version=uint16_t(h.Uint(2));if(!stats.version||stats.version>8)Fail("unsupported file version");
    if(h.Uint(2)!=1)Fail("unsupported compression flags");const auto table=h.Uint(8);if(table<64||table>=reader.size)Fail("invalid entity table offset");
    const uint64_t recordSize=16+12+16+2+(stats.version>=3?1:0)+(stats.version>=4?24:0)+(stats.version>=5?1:0)+8+4;
    const auto prefixSize=stats.version>=7?33:4;auto prefix=reader.Bytes(table,prefixSize);Cursor prefixReader{prefix};
    if(stats.version>=7){stats.hasPlayerPose=(prefixReader.Uint(1)&1)!=0;
        for(int j=0;j<3;++j)stats.playerPosition[j]=prefixReader.Float();for(int j=0;j<4;++j)stats.playerRotation[j]=prefixReader.Float();}
    stats.entities=prefixReader.Uint(4);if(stats.entities>1000000||stats.entities>(reader.size-table-prefixSize)/recordSize)Fail("invalid entity count");
    std::vector<Entity> entities;std::set<std::array<uint32_t,4>> guids;
    glm::dvec3 minimum(std::numeric_limits<double>::max()),maximum(-std::numeric_limits<double>::max());
    for(size_t i=0;i<stats.entities;++i){auto bytes=reader.Bytes(table+prefixSize+i*recordSize,recordSize);Cursor c{bytes};std::array<uint32_t,4> guid{};for(auto& x:guid)x=uint32_t(c.Uint(4));
        if(guid==std::array<uint32_t,4>{}||!guids.insert(guid).second)Fail("invalid/duplicate entity GUID");
        glm::dvec3 position;for(int k=0;k<3;++k){position[k]=c.Float();if(!std::isfinite(position[k]))Fail("nonfinite entity position");}
        glm::dquat rotation;rotation.x=c.Float();rotation.y=c.Float();rotation.z=c.Float();rotation.w=c.Float();
        const auto length=glm::length(rotation);if(!std::isfinite(length)||length<1e-8)Fail("invalid entity quaternion");rotation/=length;
        c.Skip(2);if(stats.version>=3)c.Skip(1);if(stats.version>=4)c.Skip(24);if(stats.version>=5)c.Skip(1);
        const auto index=c.Uint(8),count=c.Uint(4);if(index<64||index>reader.size||count>(reader.size-index)/24)Fail("invalid sector index");
        Entity entity;entity.transform=glm::translate(glm::dmat4(1),position)*glm::mat4_cast(rotation);
        auto entries=reader.Bytes(index,count*24);Cursor e{entries};
        for(size_t j=0;j<count;++j){Coord coord;for(auto& k:coord)k=std::bit_cast<int32_t>(uint32_t(e.Uint(4)));Region region{e.Uint(8),uint32_t(e.Uint(4))};
            if(region.offset<64||region.offset>reader.size||region.size>reader.size-region.offset)Fail("invalid sector extent");
            if(!entity.regions.emplace(coord,region).second)Fail("duplicate sector coordinates");
            for(int corner=0;corner<8;++corner){glm::dvec3 p;for(int k=0;k<3;++k)p[k]=double(coord[k])*128+((corner&(1<<k))?128:0);
                const auto world=glm::dvec3(entity.transform*glm::dvec4(p,1));minimum=glm::min(minimum,world);maximum=glm::max(maximum,world);}
        }
        stats.sectors+=count;entities.push_back(std::move(entity));
    }
    if(!stats.sectors)Fail("world has no sectors");stats.minimum=minimum;stats.maximum=maximum;stats.center=(minimum+maximum)*.5;
    size_t completed=0;
    for(const auto& entity:entities){
        struct Cached {std::shared_ptr<Sector> sector;uint64_t stamp;};std::map<Coord,Cached> cache;uint64_t stamp=0;
        auto get=[&](const Coord& coord)->std::shared_ptr<Sector>{const auto source=entity.regions.find(coord);if(source==entity.regions.end())return {};
            auto found=cache.find(coord);if(found!=cache.end()){found->second.stamp=++stamp;return found->second.sector;}
            auto sector=reader.Decode(source->second);if(cache.size()>=64){auto oldest=std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b){return a.second.stamp<b.second.stamp;});cache.erase(oldest);}
            cache.emplace(coord,Cached{sector,++stamp});return sector;};
        std::map<Coord,size_t> tiles;
        auto half=[](int32_t x){return x/2-(x%2<0?1:0);};
        for(const auto& [coord,region]:entity.regions)++tiles[Coord{half(coord[0]),half(coord[1]),half(coord[2])}];
        struct Pending {std::future<Mesh> mesh;glm::mat4 transform;};std::deque<Pending> pending;
        static const size_t workers=[](){const char* v=std::getenv("MIKAN_CXW_WORKERS");return size_t(v?std::clamp(std::atoi(v),1,16):std::min(4u,std::max(1u,std::thread::hardware_concurrency())));}();
        auto finish=[&](){auto mesh=pending.front().mesh.get();mesh.transform=pending.front().transform;pending.pop_front();
            stats.bricks+=mesh.bricks;stats.voxels+=mesh.voxels;stats.quads+=mesh.quads.size();stats.exposedFaces+=mesh.exposedFaces;
            stats.hiddenFaces+=mesh.hiddenFaces;stats.greedyQuads+=mesh.greedyQuads;completed+=mesh.sourceSectors;
            if(!mesh.quads.empty()&&consume)consume(std::move(mesh));
            if(progress)progress(completed,size_t(stats.sectors));
        };
        for(const auto& [coord,sectorCount]:tiles){
            CxwTile tile;
            for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<4;++x){
                const int outside=(x==0||x==3)+(y==0||y==3)+(z==0||z==3);if(outside>1)continue;
                const int64_t sx=int64_t(coord[0])*2+x-1,sy=int64_t(coord[1])*2+y-1,sz=int64_t(coord[2])*2+z-1;
                if(sx<INT32_MIN||sx>INT32_MAX||sy<INT32_MIN||sy>INT32_MAX||sz<INT32_MIN||sz>INT32_MAX)continue;
                tile.sectors[x+4*y+16*z]=get(Coord{int32_t(sx),int32_t(sy),int32_t(sz)});
            }
            glm::dvec3 origin;for(int k=0;k<3;++k)origin[k]=double(coord[k])*256;
            auto transform=glm::mat4(glm::translate(glm::dmat4(1),-stats.center)*entity.transform*glm::translate(glm::dmat4(1),origin));
            pending.push_back({std::async(std::launch::async,[tile=std::move(tile),mergeColors](){return Build(tile,mergeColors);}),transform});
            if(pending.size()>=workers)finish();
        }
        while(!pending.empty())finish();
    }
    return stats;
}
} // namespace CxwFormat
