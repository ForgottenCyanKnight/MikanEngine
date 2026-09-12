#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

class ModelRenderer;
struct SubMeshRenderData;

extern uint32_t g_BoneDynamicOffset;

void PushSubMeshMaterialParams(VkCommandBuffer cmd, VkPipelineLayout layout,
                               const SubMeshRenderData& sm, bool forceDoubleSided = false);

namespace ModelRendererDetail {

// Instance upload storage is intentionally kept outside ModelRenderData so the
// shared render-data layout remains stable across rendering modules.
void ReleaseInstanceUploads(const ModelRenderer* renderer);

} // namespace ModelRendererDetail
