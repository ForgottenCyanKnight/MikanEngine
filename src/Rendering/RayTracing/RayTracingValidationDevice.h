#pragma once
#include <vulkan/vulkan.h>
#include <string>
namespace VoxRTValidation {
void Require(bool condition,const char* message);
void Check(VkResult result,const char* message);
// Dedicated headless device. Never instantiate inside an initialized engine.
class Device {
public:
    ~Device();
    void Initialize(bool requireCpu=false,bool enableValidation=false,bool fullShaders=false);
    void Shutdown();
    VkCommandBuffer Begin();
    void SubmitAndWait(VkCommandBuffer cmd);
    std::string name;
    uint32_t validationErrors=0,validationWarnings=0;
private:
    VkFence fence=VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
};
}
