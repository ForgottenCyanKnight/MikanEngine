#include "Core/ProjectManager.h"
#include "Core/Log.h"
#include "zlib/zlib.h"
#include <array>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
uint64_t BlasCacheHash(const void* data,size_t size,uint64_t hash=1469598103934665603ull){
    const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i){hash^=bytes[i];hash*=1099511628211ull;}return hash;
}
constexpr uint64_t BlasCacheMagic=0x3145484341435342ull;
struct BlasCacheWrite {std::string path;std::vector<uint32_t> key;std::vector<uint8_t> data;};
// One CPU writer, bounded queue. No Vulkan calls, no wait on the rendering thread.
class BlasCacheWriter {
    std::mutex mutex;std::condition_variable changed;std::deque<BlasCacheWrite> jobs;
    bool stop=false;size_t queuedBytes=0;std::thread worker;
    void Run(){
        for(;;){BlasCacheWrite job;{
            std::unique_lock lock(mutex);changed.wait(lock,[&]{return stop||!jobs.empty();});
            if(jobs.empty()&&stop)return;job=std::move(jobs.front());jobs.pop_front();queuedBytes-=job.data.size();
        }
        try{
            auto path=std::filesystem::u8path(job.path);std::filesystem::create_directories(path.parent_path());auto temp=path;temp+=".tmp";
            uLongf compressedBytes=compressBound(static_cast<uLong>(job.data.size()));
            std::vector<uint8_t> compressed(compressedBytes);
            if(compress2(compressed.data(),&compressedBytes,job.data.data(),static_cast<uLong>(job.data.size()),Z_DEFAULT_COMPRESSION)!=Z_OK)throw std::runtime_error("compression failed");
            compressed.resize(compressedBytes);
            // Version 2: zlib payload size, raw checksum, then raw byte count.
            std::array<uint64_t,6> header{BlasCacheMagic,2,job.key.size(),compressed.size(),BlasCacheHash(job.data.data(),job.data.size(),BlasCacheHash(job.key.data(),job.key.size()*4)),job.data.size()};
            std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(header.data()),sizeof(header));out.write(reinterpret_cast<const char*>(job.key.data()),job.key.size()*4);out.write(reinterpret_cast<const char*>(compressed.data()),compressed.size());out.flush();if(!out)throw std::runtime_error("write failed");out.close();
#ifdef _WIN32
            if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("publish failed");
#else
            std::filesystem::rename(temp,path);
#endif
            LOGD("[BLAS disk] saved raw=%zu compressed=%zu path=%s",job.data.size(),compressed.size(),job.path.c_str());
            static bool reportedSave=false;if(!reportedSave){LOGI("[BLAS disk] triangle BLAS cache saved; per-BLAS details at DEBUG");reportedSave=true;}
        }catch(const std::exception& e){LOGW("[BLAS disk] save skipped: %s",e.what());}
        }
    }
public:
    BlasCacheWriter():worker([this]{Run();}){}
    ~BlasCacheWriter(){{std::lock_guard lock(mutex);stop=true;}changed.notify_one();worker.join();}
    static BlasCacheWriter& Get(){static BlasCacheWriter writer;return writer;}
    bool Enqueue(BlasCacheWrite job){
        std::lock_guard lock(mutex);if(job.data.size()+queuedBytes>256ull*1024*1024)return false;
        queuedBytes+=job.data.size();jobs.push_back(std::move(job));changed.notify_one();return true;
    }
};
}

void AccelerationStructure::ConfigureDiskCache(std::span<const uint32_t> key){
    const char* option=std::getenv("MIKAN_TRIANGLE_BLAS_CACHE");
    const auto& fn=GetRayTracingFunctions();
    if((option&&option[0]=='0')||!fn.serialize||!fn.deserialize||!fn.compatibility||!fn.writeProperties||key.empty())return;
    try{
        diskKey.assign(key.begin(),key.end());
        VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&p);
        uint64_t hash=BlasCacheHash(key.data(),key.size_bytes());
        hash=BlasCacheHash(&p.vendorID,sizeof(p.vendorID),hash);hash=BlasCacheHash(&p.deviceID,sizeof(p.deviceID),hash);hash=BlasCacheHash(&p.driverVersion,sizeof(p.driverVersion),hash);hash=BlasCacheHash(p.pipelineCacheUUID,VK_UUID_SIZE,hash);
        char name[48];std::snprintf(name,sizeof(name),"%016llx.blas",static_cast<unsigned long long>(hash));
        diskPath=(std::filesystem::u8path(ProjectManager::GetInstance().GetEngineRoot())/"cache"/"triangle-blas"/name).string();
    }catch(...){diskKey.clear();diskPath.clear();}
}
void AccelerationStructure::InheritDiskCache(const AccelerationStructure& source){diskPath=source.diskPath;diskKey=source.diskKey;}

bool AccelerationStructure::RecordLoadDiskCache(VkCommandBuffer cmd){
    if(diskPath.empty()||handle||!cmd)return false;
    const auto& fn=GetRayTracingFunctions();
    try{
        std::ifstream in(std::filesystem::u8path(diskPath),std::ios::binary|std::ios::ate);if(!in)return false;
        const auto fileBytes=in.tellg();in.seekg(0);std::array<uint64_t,5> h{};in.read(reinterpret_cast<char*>(h.data()),sizeof(h));
        if(!in||h[0]!=BlasCacheMagic||(h[1]!=1&&h[1]!=2)||h[2]!=diskKey.size()||!h[3]||h[3]>1024ull*1024*1024)throw std::runtime_error("invalid header");
        uint64_t rawBytes=h[3];
        if(h[1]==2)in.read(reinterpret_cast<char*>(&rawBytes),sizeof(rawBytes));
        if(!in||rawBytes<56||rawBytes>1024ull*1024*1024||uint64_t(fileBytes)!=sizeof(h)+(h[1]==2?8:0)+h[2]*4+h[3])throw std::runtime_error("invalid payload size");
        std::vector<uint32_t> key(diskKey.size());in.read(reinterpret_cast<char*>(key.data()),key.size()*4);if(key!=diskKey)throw std::runtime_error("geometry key mismatch");
        std::vector<uint8_t> data(static_cast<size_t>(rawBytes));
        if(h[1]==2){
            std::vector<uint8_t> compressed(static_cast<size_t>(h[3]));in.read(reinterpret_cast<char*>(compressed.data()),compressed.size());
            uLongf decoded=static_cast<uLongf>(data.size());uLong consumed=static_cast<uLong>(compressed.size());
            if(!in||uncompress2(data.data(),&decoded,compressed.data(),&consumed)!=Z_OK||decoded!=data.size()||consumed!=compressed.size())throw std::runtime_error("decompression failed");
        }else in.read(reinterpret_cast<char*>(data.data()),data.size());
        if(!in||BlasCacheHash(data.data(),data.size(),BlasCacheHash(key.data(),key.size()*4))!=h[4])throw std::runtime_error("checksum mismatch");
        VkAccelerationStructureVersionInfoKHR version{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR};version.pVersionData=data.data();VkAccelerationStructureCompatibilityKHR compatible{};fn.compatibility(g_Device,&version,&compatible);
        if(compatible!=VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR)throw std::runtime_error("incompatible device serialization");
        uint64_t serialized=0,deserialized=0,pointers=0;std::memcpy(&serialized,data.data()+32,8);std::memcpy(&deserialized,data.data()+40,8);std::memcpy(&pointers,data.data()+48,8);
        if(serialized!=data.size()||!deserialized||deserialized>1024ull*1024*1024||pointers)throw std::runtime_error("invalid BLAS serialization dimensions");
        if(!cacheUpload.Create(data.size()+255,VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
        const auto raw=cacheUpload.GetDeviceAddress();if(!raw){cacheUpload.Cleanup();return false;}const auto offset=(256-raw%256)%256;cacheUpload.Write(data.data(),data.size(),offset);
        if(!storage.Create(deserialized,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){cacheUpload.Cleanup();return false;}
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};create.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;create.buffer=storage.GetBuffer();create.size=deserialized;
        if(fn.create(g_Device,&create,g_Allocator,&handle)!=VK_SUCCESS){Cleanup();return false;}
        VkAccelerationStructureDeviceAddressInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};info.accelerationStructure=handle;address=fn.getAddress(g_Device,&info);if(!address){Cleanup();return false;}
        VkMemoryBarrier input{VK_STRUCTURE_TYPE_MEMORY_BARRIER};input.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;input.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,0,1,&input,0,nullptr,0,nullptr);
        VkCopyMemoryToAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR};copy.src.deviceAddress=raw+offset;copy.dst=handle;copy.mode=VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR;fn.deserialize(cmd,&copy);RayTracingBuildBarrier(cmd);
        type=create.type;flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;cacheStage=CacheStage::Done;
        LOGD("[BLAS disk] hit: serialized=%zu restored=%llu; build/compaction skipped",data.size(),static_cast<unsigned long long>(deserialized));
        // Upgrade legacy files after a verified hit; compression stays on the CPU writer.
        if(h[1]==1)BlasCacheWriter::Get().Enqueue({diskPath,std::move(key),std::move(data)});
        static bool reportedHit=false;if(!reportedHit){LOGI("[BLAS disk] compatible cache hit; triangle BLAS build/compaction skipped");reportedHit=true;}return true;
    }catch(const std::exception& e){LOGW("[BLAS disk] rejected; rebuilding: %s",e.what());return false;}
}

bool AccelerationStructure::RecordSaveDiskCache(VkCommandBuffer cmd,uint64_t serial){
    if(diskPath.empty()||!handle||!cmd||type!=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR)return false;
    const auto& fn=GetRayTracingFunctions();
    if(cacheStage==CacheStage::Idle){
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR;q.queryCount=1;
        if(vkCreateQueryPool(g_Device,&q,g_Allocator,&cacheQuery)!=VK_SUCCESS){cacheStage=CacheStage::Done;return false;}
        vkCmdResetQueryPool(cmd,cacheQuery,0,1);RayTracingBuildBarrier(cmd);fn.writeProperties(cmd,1,&handle,q.queryType,cacheQuery,0);cacheStage=CacheStage::Query;cacheSerial=serial;return true;
    }
    if(cacheStage!=CacheStage::Ready)return false;
    if(!cacheBytes||cacheBytes>256ull*1024*1024||!cacheReadback.Create(cacheBytes+255,VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){cacheStage=CacheStage::Done;return false;}
    const auto raw=cacheReadback.GetDeviceAddress();if(!raw){cacheStage=CacheStage::Done;return false;}cacheOffset=(256-raw%256)%256;
    VkCopyAccelerationStructureToMemoryInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR};copy.src=handle;copy.dst.deviceAddress=raw+cacheOffset;copy.mode=VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;RayTracingBuildBarrier(cmd);fn.serialize(cmd,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    cacheStage=CacheStage::Copy;cacheSerial=serial;return true;
}
void AccelerationStructure::CompleteDiskCacheAfterFence(uint64_t completedSerial){
    if(cacheSerial!=completedSerial)return;
    if(cacheStage==CacheStage::Query){
        const auto result=vkGetQueryPoolResults(g_Device,cacheQuery,0,1,sizeof(cacheBytes),&cacheBytes,sizeof(cacheBytes),VK_QUERY_RESULT_64_BIT);
        if(result==VK_NOT_READY)return;
        vkDestroyQueryPool(g_Device,cacheQuery,g_Allocator);cacheQuery=VK_NULL_HANDLE;cacheStage=result==VK_SUCCESS?CacheStage::Ready:CacheStage::Done;
    }else if(cacheStage==CacheStage::Copy){
        cacheReadback.Map();const auto* mapped=static_cast<const uint8_t*>(cacheReadback.GetMappedPtr());
        if(mapped){BlasCacheWrite job{diskPath,diskKey,std::vector<uint8_t>(mapped+cacheOffset,mapped+cacheOffset+cacheBytes)};
            if(!BlasCacheWriter::Get().Enqueue(std::move(job)))LOGD("[BLAS disk] writer queue full; save skipped");}
        cacheReadback.Unmap();cacheReadback.Cleanup();cacheStage=CacheStage::Done;
    }
}
