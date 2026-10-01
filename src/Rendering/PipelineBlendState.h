#pragma once
#include "Rendering/RendererBase.h"
#include <vector>
// Internal sidecar keeps the exported pipeline/config ABI unchanged.
void SetPipelineAttachmentBlendEnabled(VulkanPipeline* pipeline, const std::vector<VkBool32>& enabled);
