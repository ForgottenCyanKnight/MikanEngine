// All model triangle queries use the same MASK test as primary visibility.
// Opaque BLAS instances commit in hardware; only MASK instances yield candidates.
// BLEND submeshes (flag 16) use stochastic coverage instead of the 0.5 binary
// cut: accept probability equals the texel alpha, and temporal accumulation
// (TAA / RR) converges the authored soft gradient exactly. The CPU side keeps
// every BLEND triangle in the BLAS as a candidate (no opacity partition).
// alphaStochasticSeed is assigned by each including shader before tracing and
// must vary per pixel and per frame so the dither averages out over time.
uint alphaStochasticSeed=0u;
uint alphaHash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
bool acceptModelAlphaCanonical(uint index,uint primitive,vec2 bary){
    Instance instance=instances[index];
    const bool stochastic=(instance.info.y&16u)!=0u;
    if((instance.info.y&4u)==0u || instance.info.z!=1u)return true;
    uint slot=instance.info.w;
    if(slot==0xffffffffu)return true;
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
    float alpha=textureLod(modelAlbedo[nonuniformEXT(slot&0x7fffffffu)],uv,0).a;
    if(stochastic)return alpha>float(alphaHash(alphaStochasticSeed^alphaHash(primitive+0x9e3779b9u))>>8)*(1.0/16777216.0);
    return alpha>=0.5;
}
bool acceptModelAlpha(uint index,uint primitive,vec2 bary,uint asGeometry){
    Instance instance=instances[index];
    if(instance.info.z!=1u)return true;
    return acceptModelAlphaCanonical(index,modelCanonicalPrimitive(instance.info.x,asGeometry,primitive),bary);
}
