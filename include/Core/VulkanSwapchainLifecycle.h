#pragma once

// At a GPU-idle point, resize presentation resources. Rebuild offscreen targets
// only if their requested preset differs from their allocated dimensions.
void RecreateSwapChain(int width, int height);
