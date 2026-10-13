// All model triangle queries use the same MASK test as primary visibility.
// Opaque BLAS instances commit in hardware; only MASK instances yield candidates.
bool acceptModelAlpha(uint index,uint primitive,vec2 bary,uint asGeometry){
    Instance instance=instances[index];
    if((instance.info.y&4u)==0u || instance.info.z!=1u)return true;
    uint slot=instance.info.w;
    if(slot==0xffffffffu)return true;
    primitive=modelCanonicalPrimitive(instance.info.x,asGeometry,primitive);
    ModelGeometry geometry=modelGeometries[instance.info.x];
    // Partitioned BLAS candidates are already the uncertain group: opaque
    // triangles commit in hardware, transparent triangles are absent.
    // Keep the classification shortcut only for unpartitioned compatibility paths.
    if((geometry.counts.w&6u)==2u){
        ModelWords indices=ModelWords(geometry.addresses.zw);
        uint state=(indices.words[geometry.counts.y+(primitive>>4u)]>>((primitive&15u)*2u))&3u;
        if(state==1u)return true;
        if(state==2u)return false;
    }
    vec2 uv=modelUV(instance.info.x,primitive,bary);
    return textureLod(modelAlbedo[nonuniformEXT(slot&0x7fffffffu)],uv,0).a>=0.5;
}
