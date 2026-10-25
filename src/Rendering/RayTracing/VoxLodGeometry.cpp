#include "Rendering/RayTracing/VoxLodGeometry.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/Log.h"
#include <bit>
bool VoxLodGeometry::RecordBuild(VkCommandBuffer cmd,const mikan::rt::VoxLodLevel& level){
    const size_t count=level.triangles.size();if(!count||count>GetRayTracingDeviceCapabilities().limits.maxPrimitiveCount||count>UINT32_MAX/3)return false;
    std::vector<uint32_t> key{0x4c4f4442u,1u,uint32_t(count)};key.reserve(count*9+3);
    std::vector<float> vertices;vertices.reserve(count*9);std::vector<uint32_t> ids;ids.reserve(count*3);
    for(const auto& t:level.triangles){for(float v:t.positions){vertices.push_back(v);key.push_back(std::bit_cast<uint32_t>(v));}for(int k=0;k<3;++k)ids.push_back(uint32_t(ids.size()));}
    blas->ConfigureDiskCache(key);if(blas->RecordLoadDiskCache(cmd))return true;
    constexpr auto usage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if(!positions.Create(vertices.size()*4,usage,host)||!indices.Create(ids.size()*4,usage,host))return false;
    positions.Write(vertices.data(),vertices.size()*4);indices.Write(ids.data(),ids.size()*4);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& t=geometry.geometry.triangles;t.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;t.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;
    t.vertexData.deviceAddress=positions.GetDeviceAddress();t.vertexStride=12;t.maxVertex=uint32_t(ids.size()-1);t.indexType=VK_INDEX_TYPE_UINT32;t.indexData.deviceAddress=indices.GetDeviceAddress();
    RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT);
    VkAccelerationStructureBuildRangeInfoKHR range{uint32_t(count),0,0,0};
    const bool ok=blas->RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,{&geometry,1},{&range,1},VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
    if(ok)LOGI("[VOX LOD BLAS] triangles=%zu bytes=%llu",count,(unsigned long long)blas->StorageBytes());return ok;
}
