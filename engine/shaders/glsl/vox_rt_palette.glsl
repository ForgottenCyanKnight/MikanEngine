// Shared by the path tracer and RR guide producer. Binding 19 stores palette
// words and tightly packed R8 indices; indices are never filtered or mipmapped.
vec2 voxQuadCornerUV(uint face,uint corner){
    const vec2 a[4]=vec2[4](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,1));
    const vec2 b[4]=vec2[4](vec2(0,0),vec2(0,1),vec2(1,1),vec2(1,0));
    const vec2 c[4]=vec2[4](vec2(1,0),vec2(1,1),vec2(0,1),vec2(0,0));
    const vec2 d[4]=vec2[4](vec2(0,1),vec2(0,0),vec2(1,0),vec2(1,1));
    return face==0u?a[corner]:(face==2u?c[corner]:(face==5u?d[corner]:b[corner]));
}
void voxPaletteAttributes(uvec4 range,uint primitive,vec2 bary,out uint appearance,out uint material){
    bool directUV=(primitive&0x40000000u)!=0u;primitive&=0x3fffffffu;
    uint qi=range.x+primitive/2u;uvec2 decoded=voxDecodeQuad(qi);appearance=decoded.y;material=quadMaterials[qi];
    if(range.w==0u)return;
    uint header=range.w-1u,palette=quadMaterials[header],descriptors=quadMaterials[header+1u],data=quadMaterials[header+2u];
    uint first=quadMaterials[header+3u],descriptor=quadMaterials[descriptors+qi-first];
    uint id=descriptor&0x7fffffffu;
    if((descriptor&0x80000000u)!=0u){
        uint w=(decoded.x>>24)+1u,h=(decoded.y&255u)+1u;
        uint c1=(primitive&1u)==0u?2u:3u,c2=(primitive&1u)==0u?1u:2u;
        vec2 uv=voxQuadCornerUV(range.z,0u)*(1.0-bary.x-bary.y)+voxQuadCornerUV(range.z,c1)*bary.x+voxQuadCornerUV(range.z,c2)*bary.y;
        if(directUV)uv=bary;
        uvec2 cell=uvec2(clamp(floor(uv*vec2(w,h)),vec2(0),vec2(w-1u,h-1u)));
        uint byteIndex=id+cell.y*w+cell.x;
        id=(quadMaterials[data+byteIndex/4u]>>((byteIndex&3u)*8u))&255u;
    }
    appearance=(appearance&255u)|quadMaterials[palette+id*2u];
    material=quadMaterials[palette+id*2u+1u];
}
