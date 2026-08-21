#pragma once

#include "ModelRenderer.h"
#include <memory>

class PreviewModelRenderer : public ModelRenderer {
public:
    PreviewModelRenderer() : ModelRenderer() {}
    virtual ~PreviewModelRenderer() {}
    
    virtual void Init(VkRenderPass renderPass) override;
    
private:
    virtual void CreatePipeline(VkRenderPass renderPass) override;
};
