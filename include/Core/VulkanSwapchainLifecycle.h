#pragma once

// Rebuild all swapchain-dependent render targets and pipelines at a GPU-idle
// point. The Vulkan manager keeps only the global context and shutdown path.
void RecreateSwapChain(int width, int height);
