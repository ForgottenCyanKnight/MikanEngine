// NVIDIA STBN vector2, 128x128 spatial samples and 64 temporal slices.
// RGBA8 bytes are stored verbatim in a uint SSBO (little-endian desktop/ARM).
// No filtering/sRGB decode. Every stream is a fixed toroidal translation of
// the official sequence; no per-pixel scrambling or random temporal reseeding.
layout(std430,binding=17) readonly buffer StbnSamples {uint stbnSamples[];};
uint hashRandom(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float randomFloat(inout uint state){state=hashRandom(state);return float(state>>8)*(1.0/16777216.0);}
struct RaySampleState {uvec2 pixel;uint frame;uint dimension;uint fallback;};
RaySampleState initRaySampleState(uvec2 pixel,uint frame){
    return RaySampleState(pixel,frame,0u,
        hashRandom(pixel.x+pixel.y*uint(pc.extent.x))^hashRandom(frame+1u)
            ^(temporal.rtxdiParams.x<1.5&&temporal.rtxdiParams.w>0.0?hashRandom(uint(temporal.rtxdiParams.w)):0u));
}
vec2 nextRaySample(inout RaySampleState state){
    uint dimension=state.dimension++;
    if(neeR2Sampling && temporal.rtxdiParams.x<0.0){
        // Randomly shifted 2D R2 sequence across frames. Separate shifts per
        // pixel/dimension/seed; no spatial or radiance history is reused.
        uint key=hashRandom(state.pixel.x+state.pixel.y*uint(pc.extent.x))^hashRandom(dimension+0x9e3779b9u)^hashRandom(uint(temporal.rtxdiParams.w));
        // Bound float magnitude to retain fractional precision in long runs.
        // Each 1024-frame block gets a new independent shift; block0 retains
        // the original screening sequence exactly.
        uint block=state.frame>>10u;if(block>0u)key^=hashRandom(block);
        vec2 shift=vec2(randomFloat(key),randomFloat(key));
        return fract(shift+float((state.frame&1023u)+1u)*vec2(.7548776662466927,.5698402909980532));
    }
    if(temporal.options.z<.5||(uint(temporal.giOptions.w)&32768u)!=0u){
        // Experimental/reference white-noise sampling must retain the stream
        // dimension separation used by STBN (sun starts at dimension 8).
        // Scramble before advancing, also avoiding the zero-state fixed point.
        if((uint(temporal.giOptions.w)&32768u)!=0u)
            state.fallback=hashRandom(state.fallback^hashRandom(dimension+0x9e3779b9u));
        float u=randomFloat(state.fallback),v=randomFloat(state.fallback);
        return vec2(u,v);
    }
    // Fixed per-dimension offsets preserve each stream's spatial and temporal
    // blue-noise distribution. Streams are decorrelated translations, not
    // guaranteed independent high-dimensional samples.
    uvec2 xy=(state.pixel+dimension*uvec2(37u,73u))&uvec2(127u);
    uint slice=(state.frame+dimension*17u)&63u;
    uint packed=stbnSamples[(slice*128u+xy.y)*128u+xy.x];
    return (vec2(packed&255u,(packed>>8)&255u)+.5)*(1.0/256.0);
}
