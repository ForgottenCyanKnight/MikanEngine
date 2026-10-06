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
    void Initialize();
    VkCommandBuffer Begin();
    void SubmitAndWait(VkCommandBuffer cmd);
    std::string name;
private:
    VkFence fence=VK_NULL_HANDLE;
};
}
