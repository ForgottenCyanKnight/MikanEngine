#pragma once
#include <cstdint>
#include <vector>

// Raster buffer: geometry words, one header pointer per quad, attribute blocks,
// then a footer holding the pointer-table offset. RT relocates the same blocks
// into its scene material buffer instead; neither consumer runs meshing.
inline void AppendVoxRasterAttributes(std::vector<uint32_t>& words,
    const std::vector<uint32_t>& attributes,uint32_t first,uint32_t count,uint32_t lookup) {
    if(attributes.empty())return;
    const uint32_t base=uint32_t(words.size());
    words.insert(words.end(),attributes.begin(),attributes.end());
    for(uint32_t k=0;k<3;++k)words[base+k]+=base;
    words[base+3]=first;
    for(uint32_t q=first;q<first+count;++q)words[lookup+q]=base+1u;
}

template<class Renderer,class World,class Group>
inline void ConfigureVoxSurfaceMerging(Renderer& renderer,const World& world,const Group& group){
    bool uniform=false;
    for(auto id:group.entities){const auto* entity=world.Find(id);if(entity&&entity->hasMaterial&&entity->material.emissiveIntensity>0.0f){uniform=true;break;}}
    renderer.SetUniformSurfaceMaterials(uniform);
}