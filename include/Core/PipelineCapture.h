#pragma once
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace Core {
// Render-thread-only, one-shot snapshots; the caller supplies the exact live layout.
class MIKAN_API PipelineCapture {
public:
    static PipelineCapture& GetInstance();
    bool Request(uint32_t viewSlot = 0, const std::string& directory = {});
    bool Wants(uint64_t serial, uint32_t viewSlot);
    void Record(VkCommandBuffer cmd, VkImage image, VkFormat format, VkImageLayout layout,
                uint32_t width, uint32_t height, uint64_t serial, uint32_t viewSlot,
                const char* name, const char* shader, const char* interpretation);
    void Finalize(); // Only after submitting the command buffer containing the copies.
    void Shutdown(); // Device idle, before destruction.
    bool Busy() const;
    const std::string& Status() const;
    const std::string& ReportPath() const;
};
}
