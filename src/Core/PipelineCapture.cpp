// Native Vulkan adaptation of Vibris' named-resource / after-pass artifact approach.
// Reference: Luna5ama/vibris e8e26103519a2b173c7d029bfc7b0a282aa6bf76.
#include "Core/PipelineCapture.h"
#include "Core/VulkanContext.h"
#include "Core/ProjectManager.h"
#include "Core/Utf8Path.h"
#include "Core/Log.h"
#include "Rendering/RendererBase.h"
#include "json.hpp"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

namespace Core {
namespace {
using Json = nlohmann::json;
struct Item {
    VulkanBuffer staging;
    uint32_t width=0,height=0,channels=0,bytes=0;
    VkFormat format=VK_FORMAT_UNDEFINED;
    std::string name,shader,interpretation;
};
struct State {
    bool armed=false,environmentRead=false,failed=false;
    uint32_t view=0; uint32_t remaining=1;
    uint64_t serial=UINT64_MAX,target=0,totalBytes=0;
    std::string directory,status="Idle",report;
    std::vector<std::unique_ptr<Item>> items;
};
State& S(){static State state;return state;}
bool Layout(VkFormat format,uint32_t& channels,uint32_t& bytes){
    switch(format){
    case VK_FORMAT_R32_SFLOAT:channels=1;bytes=4;return true;
    case VK_FORMAT_R16G16_SFLOAT:channels=2;bytes=2;return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:channels=4;bytes=2;return true;
    case VK_FORMAT_R32G32B32A32_SFLOAT:channels=4;bytes=4;return true;
    default:return false;
    }
}
float Half(uint16_t bits){
    const int exponent=(bits>>10)&31;const int mantissa=bits&1023;
    float value=exponent==0?std::ldexp(float(mantissa),-24):
        exponent==31?(mantissa?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity()):
        std::ldexp(float(1024+mantissa),exponent-25);
    return bits&0x8000?-value:value;
}
float Read(const uint8_t* data,size_t index,uint32_t bytes){
    if(bytes==2){uint16_t value;std::memcpy(&value,data+index*2,2);return Half(value);}
    float value;std::memcpy(&value,data+index*4,4);return value;
}
uint8_t Preview(float value,const std::string& mode){
    if(!std::isfinite(value))return 255;
    if(mode=="signed")value=.5f+.5f*value;
    else if(mode=="depth")value=std::log2(1+std::max(value,0.f))/16.f;
    else if(mode=="hdr"){value=std::max(value,0.f);value=std::pow(value/(1+value),1/2.2f);}
    return uint8_t(std::clamp(value,0.f,1.f)*255+.5f);
}
bool Save(Item& item,const std::filesystem::path& root,Json& metadata){
    item.staging.Map();const auto* data=static_cast<const uint8_t*>(item.staging.GetMappedPtr());
    if(!data)return false;
    const auto raw=root/(item.name+".bin");
    std::ofstream stream(raw,std::ios::binary);stream.write(reinterpret_cast<const char*>(data),static_cast<std::streamsize>(item.staging.GetSize()));stream.close();
    if(!stream)return false;
    std::array<double,4> sum{};std::array<uint64_t,4> finite{},nonfinite{},negative{},zero{};
    std::array<float,4> minimum{},maximum{};minimum.fill(std::numeric_limits<float>::infinity());maximum.fill(-std::numeric_limits<float>::infinity());
    const size_t pixels=size_t(item.width)*item.height;
    for(size_t p=0;p<pixels;++p)for(uint32_t c=0;c<item.channels;++c){
        float v=Read(data,p*item.channels+c,item.bytes);
        if(!std::isfinite(v)){++nonfinite[c];continue;}
        ++finite[c];negative[c]+=v<0;zero[c]+=v==0;sum[c]+=v;minimum[c]=std::min(minimum[c],v);maximum[c]=std::max(maximum[c],v);
    }
    metadata={{"name",item.name},{"shader",item.shader},{"interpretation",item.interpretation},
        {"width",item.width},{"height",item.height},{"vkFormat",int(item.format)},{"channels",item.channels},
        {"componentBytes",item.bytes},{"raw",item.name+".bin"},{"byteSize",item.staging.GetSize()},
        {"layout","tightly packed native little-endian mip0 layer0; no vertical flip"},
        {"statistics",Json::array()},{"previews",Json::array()},{"diagnostics",Json::array()}};
    for(uint32_t c=0;c<item.channels;++c){
        metadata["statistics"].push_back({{"channel",c},{"min",finite[c]?Json(minimum[c]):Json(nullptr)},
            {"max",finite[c]?Json(maximum[c]):Json(nullptr)},{"mean",finite[c]?Json(sum[c]/finite[c]):Json(nullptr)},
            {"nonfinite",nonfinite[c]},{"negative",negative[c]},{"zero",zero[c]}});
        if(nonfinite[c])metadata["diagnostics"].push_back("Channel "+std::to_string(c)+": NaN/Inf present; inspect shader arithmetic and input validity.");
    }
    if(zero[0]==pixels)metadata["diagnostics"].push_back("Channel 0 is all zero. This may be intentional; check scene coverage and pass inputs.");
    if(item.interpretation=="hdr" && (negative[0]||negative[1]||negative[2]))metadata["diagnostics"].push_back("Negative radiance detected; inspect signal encoding before treating this as a lighting defect.");
    const char* rawOnly=std::getenv("MIKAN_PIPELINE_CAPTURE_RAW_ONLY");
    if(rawOnly&&rawOnly[0]=='1')return true;
    std::vector<uint8_t> rgba(pixels*4);
    for(int selected=-1;selected<int(item.channels);++selected){
        for(size_t p=0;p<pixels;++p){
            for(int c=0;c<3;++c){const uint32_t source=selected<0?std::min(uint32_t(c),item.channels-1):uint32_t(selected);
                rgba[p*4+c]=Preview(Read(data,p*item.channels+source,item.bytes),item.interpretation);}
            rgba[p*4+3]=255;
        }
        std::string file=item.name+(selected<0?"-rgb":"-c"+std::to_string(selected))+".png";
        SDL_Surface* surface=SDL_CreateSurfaceFrom(int(item.width),int(item.height),SDL_PIXELFORMAT_RGBA32,rgba.data(),int(item.width*4));
        if(!surface)return false;bool ok=IMG_SavePNG(surface,Utf8String(root/file).c_str());SDL_DestroySurface(surface);if(!ok)return false;
        metadata["previews"].push_back(file);
    }
    return true;
}
}
PipelineCapture& PipelineCapture::GetInstance(){static PipelineCapture capture;return capture;}
bool PipelineCapture::Busy()const{return S().armed;}
const std::string& PipelineCapture::Status()const{return S().status;}
const std::string& PipelineCapture::ReportPath()const{return S().report;}
bool PipelineCapture::Request(uint32_t view,const std::string& directory){
    auto& s=S();if(s.armed)return false;
    s.armed=true;s.view=view;s.serial=UINT64_MAX;s.target=0;s.totalBytes=0;s.failed=false;
    s.directory=directory;s.report.clear();s.status="Waiting for selected RT viewport";return true;
}
bool PipelineCapture::Wants(uint64_t serial,uint32_t view){
    auto& s=S();if(!s.environmentRead){s.environmentRead=true;
        const char* frame=std::getenv("MIKAN_PIPELINE_CAPTURE_FRAME");
        if(frame){char* end=nullptr;auto value=std::strtoull(frame,&end,10);
            if(end!=frame && !*end && value>0){const char* output=std::getenv("MIKAN_PIPELINE_CAPTURE_DIR");
                const char* slot=std::getenv("MIKAN_PIPELINE_CAPTURE_VIEW");
                Request(slot&&slot[0]=='1'?1u:0u,output?output:"");s.target=value; const char* count=std::getenv("MIKAN_PIPELINE_CAPTURE_COUNT");s.remaining=count?uint32_t(std::clamp(std::atoi(count),1,32)):1u;}}
    }
    return s.armed && !s.failed && s.view==view && serial>=s.target && (s.serial==UINT64_MAX||s.serial==serial);
}
void PipelineCapture::Record(VkCommandBuffer cmd,VkImage image,VkFormat format,VkImageLayout layout,
    uint32_t width,uint32_t height,uint64_t serial,uint32_t view,const char* name,const char* shader,const char* interpretation){
    if(!Wants(serial,view))return;auto& s=S();
    const char* resource=std::getenv("MIKAN_PIPELINE_CAPTURE_RESOURCE");
    if(resource&&*resource&&std::strcmp(resource,name)!=0)return;
    for(const auto& item:s.items)if(item->name==name)return;
    uint32_t channels=0,bytes=0;const bool supported=Layout(format,channels,bytes);
    const VkDeviceSize size=VkDeviceSize(width)*height*channels*bytes;
    if(!supported||!image||!cmd||!size||size>256ull*1024*1024-s.totalBytes||s.items.size()>=32){s.failed=true;s.status="Unsupported resource or capture quota exceeded";return;}
    auto item=std::make_unique<Item>();item->width=width;item->height=height;item->channels=channels;item->bytes=bytes;
    item->format=format;item->name=name;item->shader=shader;item->interpretation=interpretation;
    if(!item->staging.Create(size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
        s.failed=true;s.status="Cannot allocate coherent readback memory";return;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=image;
    barrier.oldLayout=layout;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,item->staging.GetBuffer(),1,&copy);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=layout;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.buffer=item->staging.GetBuffer();host.size=VK_WHOLE_SIZE;
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    s.serial=serial;s.totalBytes+=size;s.items.push_back(std::move(item));s.status="Recorded; waiting for GPU submission";
}
void PipelineCapture::Finalize(){
    auto& s=S();if(!s.armed)return;
    if(s.items.empty()){if(s.failed)s.armed=false;return;}
    if(vkQueueWaitIdle(g_Queue)!=VK_SUCCESS){s.status="GPU completion failed";s.failed=true;return;}
    try{
        if(s.failed)throw std::runtime_error(s.status);
        const auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        auto root=s.directory.empty()?Utf8Path(ProjectManager::GetInstance().GetProjectRoot())/"out"/"pipeline-captures":Utf8Path(s.directory);
        root/=std::to_string(stamp)+"-view"+std::to_string(s.view)+"-frame"+std::to_string(s.serial);
        std::filesystem::create_directories(root);
        VkPhysicalDeviceProperties gpu{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&gpu);
        Json report={{"schemaVersion",1},{"kind","shader-resource-snapshot"},{"frameSerial",s.serial},{"viewSlot",s.view},
            {"gpu",gpu.deviceName},{"resources",Json::array()},
            {"limitations","One-shot GPU stall. Preview transforms only; raw data is authoritative. Heuristic diagnostics do not establish visual correctness or performance. No command replay."}};
        for(auto& item:s.items){Json metadata;if(!Save(*item,root,metadata))throw std::runtime_error("Resource export failed");report["resources"].push_back(metadata);}
        std::ofstream json(root/"report.json");json<<report.dump(2);json.close();if(!json)throw std::runtime_error("Report write failed");
        std::ofstream html(root/"index.html");
        html<<R"HTML(<!doctype html><meta charset="utf-8"><title>Mikan pipeline capture</title><style>body{background:#151820;color:#ddd;font:15px system-ui;margin:24px}select,button{padding:8px;background:#252a36;color:white}img{display:block;max-width:100%;image-rendering:pixelated;margin-top:16px}pre{white-space:pre-wrap}a{color:#8bd}</style><h1>Mikan pipeline capture</h1><p>Frozen shader resources. HDR: Reinhard + gamma; signed: 0.5 + 0.5*x; depth: log2(1+x)/16. Raw BIN preserves original values.</p><select id="resource"></select> <select id="channel"></select> <a href="report.json">JSON diagnostics</a> <a id="raw">Raw BIN</a><p><label>Load selected raw BIN for pixel inspection: <input id="binary" type="file" accept=".bin"></label></p><output id="pixel"></output><img id="image"><pre id="details"></pre><script>const report=)HTML";
        html<<report.dump(-1,' ',false,Json::error_handler_t::replace);
        html<<R"HTML(;const r=document.querySelector('#resource'),c=document.querySelector('#channel');report.resources.forEach((x,i)=>r.add(new Option(x.name,i)));function show(){const x=report.resources[r.value];document.querySelector('#image').src=x.previews[c.value||0];document.querySelector('#raw').href=x.raw;document.querySelector('#details').textContent=JSON.stringify({frame:report.frameSerial,view:report.viewSlot,gpu:report.gpu,...x,limitations:report.limitations},null,2)}function select(){c.innerHTML='';report.resources[r.value].previews.forEach((x,i)=>c.add(new Option(i?'Channel '+(i-1):'RGB',i)));show()}let bytes=null;const pixel=document.querySelector("#pixel"),img=document.querySelector("#image");document.querySelector("#binary").onchange=async e=>{const f=e.target.files[0],x=report.resources[r.value];if(!f)return;if(f.name!==x.raw||f.size!==x.byteSize){pixel.textContent="Select the matching BIN for this resource";bytes=null;return}bytes=new DataView(await f.arrayBuffer());pixel.textContent="Hover the image for original channel values"};function half(b){const e=(b>>10)&31,m=b&1023;return (b&32768?-1:1)*(e===0?m*2**-24:e===31?(m?NaN:Infinity):(1024+m)*2**(e-25))}img.onmousemove=e=>{if(!bytes)return;const t=report.resources[r.value],rect=img.getBoundingClientRect(),x=Math.min(t.width-1,Math.max(0,Math.floor((e.clientX-rect.left)*t.width/rect.width))),y=Math.min(t.height-1,Math.max(0,Math.floor((e.clientY-rect.top)*t.height/rect.height)));const v=[];for(let c=0;c<t.channels;c++){const o=((y*t.width+x)*t.channels+c)*t.componentBytes;v.push(t.componentBytes===2?half(bytes.getUint16(o,true)):bytes.getFloat32(o,true))}pixel.textContent=`(${x}, ${y}) [${v.join(", ")}]`};r.onchange=()=>{bytes=null;pixel.textContent="";select()};c.onchange=show;select();</script>)HTML";
        html.close();if(!html)throw std::runtime_error("Viewer write failed");
        s.report=Utf8String(root/"index.html");s.status="Saved "+std::to_string(s.items.size())+" resources";
        LOGI("[PipelineCapture] %s -> %s",s.status.c_str(),s.report.c_str());
    }catch(const std::exception& e){s.failed=true;s.status=e.what();LOGE("[PipelineCapture] %s",e.what());}
    s.items.clear();s.armed=false;s.totalBytes=0;
    if(!s.failed && s.remaining>1){--s.remaining;s.target=s.serial+1;s.serial=UINT64_MAX;s.armed=true;}
}
void PipelineCapture::Shutdown(){auto& s=S();s.items.clear();s.armed=false;s.totalBytes=0;}
}
