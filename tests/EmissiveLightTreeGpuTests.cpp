#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <windows.h>
#include "../src/Rendering/RayTracing/EmissiveLightTree.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace mikan::rt::lights;
#define VK_FUNCTIONS(X) \
 X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
 X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
 X(vkCreateDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
 X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateDescriptorSetLayout) \
 X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
 X(vkCreateShaderModule) X(vkCreatePipelineLayout) X(vkCreateComputePipelines) \
 X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
 X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) \
 X(vkCmdPipelineBarrier) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) \
 X(vkUnmapMemory) X(vkDestroyBuffer) X(vkFreeMemory) X(vkDestroyCommandPool) \
 X(vkDestroyPipeline) X(vkDestroyPipelineLayout) X(vkDestroyShaderModule) \
 X(vkDestroyDescriptorPool) X(vkDestroyDescriptorSetLayout) X(vkDestroyDevice) X(vkDestroyInstance)
#define DECLARE(name) static PFN_##name name;
VK_FUNCTIONS(DECLARE)
static void Require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
static void VkCheck(VkResult rc){if(rc!=VK_SUCCESS)throw std::runtime_error("Vulkan error "+std::to_string(rc));}
struct Input {glm::vec4 point;glm::uvec4 data;};
struct Buffer {VkBuffer handle;VkDeviceMemory memory;void* mapped;};
int main(int argc,char** argv){try{
    Require(argc==2||argc==3,"SPIR-V path required");const bool quality=argc==3;HMODULE loader=LoadLibraryW(L"vulkan-1.dll");Require(loader!=nullptr,"Vulkan loader missing");
#define LOAD(name) name=reinterpret_cast<PFN_##name>(GetProcAddress(loader,#name));Require(name!=nullptr,#name);
    VK_FUNCTIONS(LOAD)
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;VkInstance instance;VkCheck(vkCreateInstance(&ic,nullptr,&instance));
    uint32_t deviceCount=0;VkCheck(vkEnumeratePhysicalDevices(instance,&deviceCount,nullptr));Require(deviceCount>0,"GPU unavailable");
    std::vector<VkPhysicalDevice> devices(deviceCount);VkCheck(vkEnumeratePhysicalDevices(instance,&deviceCount,devices.data()));VkPhysicalDevice physical=devices[0];
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);
    uint32_t queueCount;vkGetPhysicalDeviceQueueFamilyProperties(physical,&queueCount,nullptr);std::vector<VkQueueFamilyProperties> queues(queueCount);vkGetPhysicalDeviceQueueFamilyProperties(physical,&queueCount,queues.data());
    uint32_t family=0;while(family<queueCount&&!(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;Require(family<queueCount,"compute queue missing");
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;VkDevice device;VkCheck(vkCreateDevice(physical,&dc,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    constexpr uint32_t count=4096;std::vector<glm::vec4> lights(1,glm::vec4(float(count),0,0,0));std::vector<HitKey> keys;
    for(uint32_t i=0;i<count;++i){float x=float(i%64)*2,y=float(i/64)*2;
        lights.emplace_back(glm::vec3(i%7==0?1e-12f:(i%11==0?1e12f:float(1+i%5))),.5f);
        lights.emplace_back(x,y,0,0);lights.emplace_back(x+1,y,0,0);lights.emplace_back(x,y+1,0,0);keys.push_back({i%3,i%6,i/3,i});}
    const bool mixture=quality&&std::string(argv[2])=="mixture";
    const bool prime=quality&&(std::string(argv[2])=="prime"||mixture);
    for(uint32_t i=0;i<count;++i){lights[2+4*i].w=float(i+1)/count;lights[3+4*i].w=1.f/count;}
    Tree tree;Require(Build(lights,count,tree,prime),"CPU build");Require(Append(lights,count,keys,prime),"pack");std::vector<Input> inputs;
    // Every leaf's first, middle and last represented random integer, for four receivers.
    for(glm::vec3 point:{glm::vec3(0),glm::vec3(.25f,.25f,0),glm::vec3(3,6,2),glm::vec3(-1e6f,1e6f,10)}){
        std::function<void(uint32_t,uint32_t,uint32_t)> visit=[&](uint32_t at,uint32_t first,uint32_t mass){const Node& n=tree.nodes[at];
            if(n.child&LeafBit){uint32_t i=n.child&~LeafBit;for(uint32_t target:{first,first+mass/2,first+mass-1})inputs.push_back({glm::vec4(point,Bits(i%6)),glm::uvec4(target,i%3,i/3,i)});return;}
            auto scoreNode=[&](Node node){if(quality&&(node.child&LeafBit)){
                uint32_t base=1+4*(node.child&~LeafBit);glm::vec3 a(lights[base+1]),b(lights[base+2]),c(lights[base+3]);
                glm::vec3 lo=glm::min(a,glm::min(b,c)),hi=glm::max(a,glm::max(b,c));
                glm::vec3 delta=glm::max(glm::max(lo-point,point-hi),glm::vec3(0));
                node.center=point;node.radius2=std::max(glm::dot(delta,delta),std::max(lights[base].w*.25f,1e-12f));}
                else if(prime)node.radius2=0;return node;};
            uint32_t left=LeftCount(scoreNode(tree.nodes[n.child]),scoreNode(tree.nodes[n.child+1]),point,mass);visit(n.child,first,left);visit(n.child+1,first+left,mass-left);
        };visit(0,0,SelectorCount);
    }
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    auto buffer=[&](VkDeviceSize bytes){Buffer b{};VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;VkCheck(vkCreateBuffer(device,&bi,nullptr,&b.handle));
        VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,b.handle,&requirements);uint32_t type=0;
        const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while(type<memory.memoryTypeCount&&(!((1u<<type)&requirements.memoryTypeBits)||(memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;
        Require(type<memory.memoryTypeCount,"coherent memory unavailable");VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;VkCheck(vkAllocateMemory(device,&ai,nullptr,&b.memory));VkCheck(vkBindBufferMemory(device,b.handle,b.memory,0));VkCheck(vkMapMemory(device,b.memory,0,bytes,0,&b.mapped));return b;};
    const VkDeviceSize bytes[3]={lights.size()*sizeof(glm::vec4),inputs.size()*sizeof(Input),inputs.size()*sizeof(glm::uvec4)};
    Buffer buffers[3]={buffer(bytes[0]),buffer(bytes[1]),buffer(bytes[2])};memcpy(buffers[0].mapped,lights.data(),size_t(bytes[0]));memcpy(buffers[1].mapped,inputs.data(),size_t(bytes[1]));memset(buffers[2].mapped,0,size_t(bytes[2]));
    VkDescriptorSetLayoutBinding bindings[3]{};for(uint32_t i=0;i<3;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=3;li.pBindings=bindings;VkDescriptorSetLayout layout;VkCheck(vkCreateDescriptorSetLayout(device,&li,nullptr,&layout));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&size;VkDescriptorPool pool;VkCheck(vkCreateDescriptorPool(device,&pi,nullptr,&pool));
    VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=pool;sa.descriptorSetCount=1;sa.pSetLayouts=&layout;VkDescriptorSet set;VkCheck(vkAllocateDescriptorSets(device,&sa,&set));
    VkDescriptorBufferInfo infos[3];VkWriteDescriptorSet writes[3]{};for(uint32_t i=0;i<3;++i){infos[i]={buffers[i].handle,0,bytes[i]};writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}vkUpdateDescriptorSets(device,3,writes,0,nullptr);
    std::ifstream file(argv[1],std::ios::binary|std::ios::ate);Require(bool(file),"SPIR-V missing");auto length=file.tellg();std::vector<uint32_t> code(size_t(length)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),length);
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=size_t(length);si.pCode=code.data();VkShaderModule shader;VkCheck(vkCreateShaderModule(device,&si,nullptr,&shader));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;VkPipelineLayout pipelineLayout;VkCheck(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout));
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.layout=pipelineLayout;cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.module=shader;cp.stage.pName="main";VkPipeline pipeline;VkCheck(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline));
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;VkCommandPool commandPool;VkCheck(vkCreateCommandPool(device,&pc,nullptr,&commandPool));
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commandPool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd;VkCheck(vkAllocateCommandBuffers(device,&ca,&cmd));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VkCheck(vkBeginCommandBuffer(cmd,&begin));
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);uint32_t inputCount=uint32_t(inputs.size());vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&inputCount);vkCmdDispatch(cmd,(inputCount+63)/64,1,1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);VkCheck(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;VkCheck(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VkCheck(vkQueueWaitIdle(queue));
    const auto* outputs=static_cast<const glm::uvec4*>(buffers[2].mapped);uint32_t roundingDifferences=0;uint64_t massSum=0;
    for(uint32_t i=0;i<inputCount;++i){Require(outputs[i].y==outputs[i].z,"GPU forward/reverse PMF mismatch");Require(std::bit_cast<float>(outputs[i].y)>0,"GPU lost support");
        if(mixture){if(i%3==1)massSum+=uint32_t(std::bit_cast<float>(outputs[i].w)*(2u*SelectorCount));
            if((i+1)%(count*3)==0){Require(massSum==2u*SelectorCount,"GPU mixture PMF not normalized");massSum=0;}continue;}
        Require(outputs[i].w==inputs[i].data.w,"GPU hit identity mismatch");
        if(outputs[i].x!=inputs[i].data.w){++roundingDifferences;Require(i%3!=1,"GPU selected wrong interior interval");}
        if(i%3==1)massSum+=uint32_t(std::bit_cast<float>(outputs[i].y)*SelectorCount);
        if((i+1)%(count*3)==0){Require(massSum==SelectorCount,"GPU PMF not normalized");massSum=0;}}
    std::cout<<"PASS GPU: "<<properties.deviceName<<"; "<<inputCount<<" production shader queries; exact forward/reverse PMF and hit identity; CPU/GPU endpoint rounding differences="<<roundingDifferences<<'\n';
    for(auto& b:buffers){vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.handle,nullptr);vkFreeMemory(device,b.memory,nullptr);}vkDestroyCommandPool(device,commandPool,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyPipelineLayout(device,pipelineLayout,nullptr);vkDestroyShaderModule(device,shader,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(loader);return 0;
}catch(const std::exception& e){std::cerr<<"FAIL GPU: "<<e.what()<<'\n';return 1;}}
