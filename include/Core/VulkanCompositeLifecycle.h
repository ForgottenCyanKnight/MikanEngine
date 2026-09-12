#pragma once

// High-level setup for the composite render targets, post-process chains and
// fullscreen descriptor bindings. Low-level render-pass/framebuffer creation
// remains in VulkanCompositeResources.
void InitCompositeResources();
void UpdateFullscreenQuadDescriptors();
