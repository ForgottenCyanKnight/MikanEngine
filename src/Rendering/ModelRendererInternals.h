#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

class ModelRenderer;
struct SubMeshRenderData;
struct ModelInstanceData;

extern uint32_t g_BoneDynamicOffset;

void PushSubMeshMaterialParams(VkCommandBuffer cmd, VkPipelineLayout layout,
                               const SubMeshRenderData& sm, bool forceDoubleSided = false);

namespace ModelRendererDetail {

// Instance upload storage is intentionally kept outside ModelRenderData so the
// shared render-data layout remains stable across rendering modules.
void ReleaseInstanceUploads(const ModelRenderer* renderer);

// Shared GPU skinning palette. Each animated renderer owns its pose on the
// CPU, but the pose is uploaded once per frame into this SSBO and addressed by
// an instance attribute. This keeps animation state independent while making
// the draw path batchable.
inline constexpr uint32_t kInvalidSharedBonePaletteBase = UINT32_MAX;
void EnsureSharedBonePalette();
void BeginSharedBonePaletteFrame();
void ReleaseSharedBonePalette();
VkBuffer GetSharedBonePaletteBuffer();
uint32_t GetSharedBonePaletteBase(const ModelRenderer* renderer);
void ApplySharedBonePalette(ModelInstanceData& instance, const ModelRenderer* renderer);

} // namespace ModelRendererDetail
