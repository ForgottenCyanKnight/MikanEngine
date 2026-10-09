layout(std430,binding=2) readonly buffer VoxSurfaceWords { uint surfaceWords[]; };
layout(location=5) flat in uint surfaceQuad;
layout(location=6) in vec2 surfaceUV;
layout(location=7) flat in vec3 surfaceTint;

bool voxSurfaceEntry(out uint rgb,out uint material) {
    uint lookup=surfaceWords[surfaceWords.length()-1];
    uint pointer=surfaceWords[lookup+surfaceQuad];
    if(pointer==0u){rgb=0u;material=0u;return false;}
    uint header=pointer-1u;
    uint palette=surfaceWords[header],descriptors=surfaceWords[header+1u],data=surfaceWords[header+2u];
    uint descriptor=surfaceWords[descriptors+surfaceQuad-surfaceWords[header+3u]];
    uint id=descriptor&0x7fffffffu;
    if((descriptor&0x80000000u)!=0u){
        uint geometry=surfaceWords[surfaceQuad];
        uint w=((geometry>>16)&255u)+1u,h=(geometry>>24)+1u;
        uvec2 cell=uvec2(clamp(floor(surfaceUV*vec2(w,h)),vec2(0),vec2(w-1u,h-1u)));
        uint index=id+cell.y*w+cell.x;
        id=(surfaceWords[data+index/4u]>>((index&3u)*8u))&255u;
    }
    rgb=surfaceWords[palette+id*2u];material=surfaceWords[palette+id*2u+1u];return true;
}
vec3 voxSurfaceColor(vec3 fallback){
    uint rgb,material;if(!voxSurfaceEntry(rgb,material))return fallback;
    return vec3((rgb>>8)&255u,(rgb>>16)&255u,(rgb>>24)&255u)/255.0*surfaceTint;
}

vec4 voxSurfaceMaterial(){
    uint rgb,m;voxSurfaceEntry(rgb,m);
    float metallic=(m&1u)!=0u?1.0:0.0,roughness=(m&1u)!=0u?0.0:1.0;
    if((m&4u)!=0u){roughness=float((m>>8)&255u)/255.0;metallic=unpackHalf2x16(m>>16).x;}
    float emission=(m&2u)!=0u?unpackHalf2x16(m>>16).x:0.0;
    return vec4(metallic,roughness,1.0,emission);
}
