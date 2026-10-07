#include "Rendering/VoxelDDARegistry.h"
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
                                    const std::vector<glm::vec4>& palette, const glm::mat4& gridToLocal) {
    std::lock_guard lock(g_Mutex);
    for (auto& grid : m_Grids)
        if (grid.name == name) {
            grid.imageView = imageView;
            grid.sizeX = sizeX; grid.sizeY = sizeY; grid.sizeZ = sizeZ;
            grid.palette = palette; grid.gridToLocal=gridToLocal;
            ++m_Version;
            return grid.id;
        }
    // Reuse freed ids so shader array indices stay dense and stable.
    uint32_t id = static_cast<uint32_t>(m_Grids.size());
    for (uint32_t candidate = 0; candidate < m_Grids.size(); ++candidate)
        if (m_Grids[candidate].imageView == nullptr) { id = candidate; break; }
    if(id>=8u)return UINT32_MAX; // excess assets retain triangle traversal
    if (id == m_Grids.size()) m_Grids.push_back({});
    auto& grid = m_Grids[id];
    grid.id = id; grid.name = name; grid.imageView = imageView;
    grid.sizeX = sizeX; grid.sizeY = sizeY; grid.sizeZ = sizeZ;
    grid.palette = palette;
    grid.gridToLocal=gridToLocal;++m_Version;return id;
}

void VoxelDDARegistry::Unregister(const std::string& name) {
    std::lock_guard lock(g_Mutex);
    for (auto& grid : m_Grids)
        if (grid.name == name) { grid.imageView = nullptr; grid.palette.clear(); break; }
    ++m_Version;
}
bool VoxelDDARegistry::BootEnabled() {
    static const bool enabled=[] {const char* v=std::getenv("MIKAN_HWRT_VOXEL_DDA");return v&&v[0]=='1';}();
    return enabled;
}

void VoxelDDARegistry::Clear() {
    std::lock_guard lock(g_Mutex);
    m_Grids.clear();++m_Version;
}

}
