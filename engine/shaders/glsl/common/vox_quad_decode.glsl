// Buffer footer: plane table offset, plane run count, raster attribute lookup.
uvec2 voxDecodeQuad(uint q){
    uint length=uint(quadWords.length()),base=quadWords[length-3u],count=quadWords[length-2u];
    uint lo=0u,hi=count;
    while(lo<hi){uint mid=(lo+hi)/2u;if(quadWords[base+mid*2u]<=q)lo=mid+1u;else hi=mid;}
    uint key=quadWords[base+(lo-1u)*2u+1u],d=key>>16,p=key&65535u;
    uint packed=quadWords[q],u=packed&255u,v=(packed>>8)&255u;
    uint xyz=d<2u?(u|(v<<8)|(p<<16)):(d<4u?(p|(v<<8)|(u<<16)):(u|(p<<8)|(v<<16)));
    return uvec2(xyz|(((packed>>16)&255u)<<24),packed>>24);
}
