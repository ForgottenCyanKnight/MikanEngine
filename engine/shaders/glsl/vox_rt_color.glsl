#ifndef VOX_RT_COLOR_GLSL
#define VOX_RT_COLOR_GLSL
// Linear Rec.709/sRGB primaries throughout shading, transport and NRD.
// Only encoded color textures/palette bytes cross this input boundary.
vec3 rtSrgbToLinear(vec3 encoded){
    encoded=max(encoded,vec3(0));
    return mix(pow((encoded+.055)/1.055,vec3(2.4)),encoded/12.92,
        lessThanEqual(encoded,vec3(.04045)));
}
vec3 rtLinearToSrgb(vec3 linearColor){
    linearColor=max(linearColor,vec3(0));
    return mix(1.055*pow(linearColor,vec3(1.0/2.4))-.055,12.92*linearColor,
        lessThanEqual(linearColor,vec3(.0031308)));
}
vec3 rtDisplayMap(vec3 hdr){
    hdr=max(hdr,vec3(0));
    // Shared peak-channel Reinhard compression preserves linear RGB ratios.
    // Bounds all channels without separate channel clipping/desaturation.
    float peak=max(hdr.r,max(hdr.g,hdr.b));
    return hdr/(1.0+peak);
}
#endif
