#include "Rendering/VoxelDDARegistry.h"
#include "Rendering/RayTracing/VoxDdaOptimization.h"
#include "Rendering/RayTracing/VoxNaadf.h"
#include "Core/Log.h"
#include "Core/VulkanContext.h"
#include <chrono>
#include <cstring>
#include <mutex>
#include <cstdlib>

namespace mikan::render {
namespace {
std::mutex g_Mutex;
}

VoxelDDARegistry& VoxelDDARegistry::Get() {
    static VoxelDDARegistry registry;
    return registry;
}

uint32_t VoxelDDARegistry::Register(const std::string& name, void* imageView,
                                    uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
                                    const std::vector<glm::vec4>& palette, const glm::mat4& gridToLocal,
                                    const std::vector<uint8_t>& brickCells) {
    std::lock_guard lock(g_Mutex);
    auto makeMips=[&](){
        if(!BitsetEnabled())return std::vector<uint32_t>{};
        auto padded=[](uint32_t n){uint32_t p=1;while(p<n)p*=2;return p;};
        if(NaadfEnabled()){
            const auto start=std::chrono::steady_clock::now();
            auto words=dda::BuildNaadf(brickCells,padded(sizeX),padded(sizeY),padded(sizeZ));
            LOGI("[NAADF] asset=%s raw=%ux%ux%u bytes=%zu build_ms=%.3f",name.c_str(),sizeX,sizeY,sizeZ,words.size()*4,
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            return words;
        }
        return dda::BuildBitsetMips(brickCells,padded(sizeX),padded(sizeY),padded(sizeZ));
    };
    for (auto& grid : m_Grids)
        if (grid.name == name) {
            grid.imageView = imageView;
            grid.sizeX = sizeX; grid.sizeY = sizeY; grid.sizeZ = sizeZ;
            grid.palette = palette; grid.gridToLocal=gridToLocal;
            grid.bitsetMips=makeMips();
            grid.brickCells=brickCells;
            ++m_Version;
            return grid.id;
        }
    // Reuse freed ids so shader array indices stay dense and stable.
    uint32_t id = static_cast<uint32_t>(m_Grids.size());
    for (uint32_t candidate = 0; candidate < m_Grids.size(); ++candidate)
        if (m_Grids[candidate].imageView == nullptr) { id = candidate; break; }
    if(id>=kVoxelDdaMaxGrids)return UINT32_MAX; // excess assets retain triangle traversal
    if (id == m_Grids.size()) m_Grids.push_back({});
    auto& grid = m_Grids[id];
    grid.id = id; grid.name = name; grid.imageView = imageView;
    grid.sizeX = sizeX; grid.sizeY = sizeY; grid.sizeZ = sizeZ;
    grid.palette = palette;
    grid.bitsetMips=makeMips();
    grid.brickCells=brickCells;
    grid.gridToLocal=gridToLocal;++m_Version;return id;
}

void VoxelDDARegistry::Unregister(const std::string& name) {
    std::lock_guard lock(g_Mutex);
    for (auto& grid : m_Grids)
        if (grid.name == name) { grid.imageView = nullptr; grid.palette.clear();grid.brickCells.clear();grid.bitsetMips.clear(); break; }
    ++m_Version;
}
bool VoxelDDARegistry::BrickEnabled() {
    static const bool enabled=[] {
        if(const char* b=std::getenv("MIKAN_VOX_HW_BRICK_DDA"))return b[0]=='1';
        // Explicit software-traversal requests keep their whole-volume semantics.
        if(const char* v=std::getenv("MIKAN_HWRT_VOXEL_DDA"))if(v[0]=='1')return false;
        if(BitsetEnabled())return false;
        return false; // Hardware triangles by default; DDA paths are opt-in.
    }();
    return enabled;
}
bool VoxelDDARegistry::BootEnabled() {
    static const bool enabled=[] {const char* v=std::getenv("MIKAN_HWRT_VOXEL_DDA");return (v&&v[0]=='1')||BrickEnabled()||BitsetEnabled();}();
    return enabled;
}
bool VoxelDDARegistry::ScreenBoundsEnabled(){
    static const bool enabled=[] {const char* v=std::getenv("MIKAN_HWRT_DDA_SCREEN_BOUNDS");return v&&v[0]=='1';}();
    return enabled;
}
bool VoxelDDARegistry::BitsetEnabled(){
    static const bool enabled=[] {const char* v=std::getenv("MIKAN_HWRT_DDA_BITSET");return (v&&v[0]=='1')||ScreenBoundsEnabled()||NaadfEnabled();}();
    return enabled;
}
bool VoxelDDARegistry::NaadfEnabled(){
    static const bool enabled=[] {const char* v=std::getenv("MIKAN_HWRT_DDA_NAADF");return v&&std::strcmp(v,"1")==0;}();
    return enabled;
}

void VoxelDDARegistry::Clear() {
    std::lock_guard lock(g_Mutex);
    m_Grids.clear();++m_Version;
}

}
