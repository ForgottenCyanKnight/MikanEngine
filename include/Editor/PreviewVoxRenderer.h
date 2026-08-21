#pragma once

#include "Rendering/VoxRenderer.h"
#include <memory>

class PreviewVoxRenderer : public VoxRenderer {
public:
    PreviewVoxRenderer() : VoxRenderer() {}
    virtual ~PreviewVoxRenderer() {}
    
    virtual void Init(VkRenderPass renderPass) override;
    
private:
    virtual void CreatePipeline(VkRenderPass renderPass) override;
};