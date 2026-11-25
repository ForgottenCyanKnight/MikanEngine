// Shared geometry record decoder for ray tracing and reconstruction guides.
layout(std430,binding=40) readonly buffer DdaGridData {uint ddaWords[];};
mat4 ddaWorldToGrid(uint record){
    uint b=16u+record*24u;
    return mat4(uintBitsToFloat(uvec4(ddaWords[b],ddaWords[b+1u],ddaWords[b+2u],ddaWords[b+3u])),
        uintBitsToFloat(uvec4(ddaWords[b+4u],ddaWords[b+5u],ddaWords[b+6u],ddaWords[b+7u])),
        uintBitsToFloat(uvec4(ddaWords[b+8u],ddaWords[b+9u],ddaWords[b+10u],ddaWords[b+11u])),
        uintBitsToFloat(uvec4(ddaWords[b+12u],ddaWords[b+13u],ddaWords[b+14u],ddaWords[b+15u])));
}
uint ddaPaletteWord(uint record,uint voxel){return 16u+ddaWords[0]*24u+(ddaWords[16u+record*24u+20u]+voxel)*4u;}
