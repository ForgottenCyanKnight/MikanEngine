#version 450
#extension GL_GOOGLE_include_directive : require
#include "vox_rt_color.glsl"
layout(location=0) in vec2 fragUV;
layout(binding=0) uniform sampler2D hardwareImage;
layout(location=0) out vec4 outColor;
uint displayHash(uint x){
    x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;x^=x>>16u;
    return x;
}
void main(){
    vec3 hdr=max(texture(hardwareImage,fragUV).rgb,vec3(0));
    // Current editor display and requested swapchain attachments are UNORM:
    // encode exactly once here. A future SRGB attachment must skip this encode.
    vec3 encoded=rtLinearToSrgb(rtDisplayMap(hdr));
    // Static, zero-mean +/- half an 8-bit code value in encoded display space.
    // Shared RGB noise preserves neutral hues; final-only placement keeps it
    // out of denoising/TAA history. No STBN resource or frame uniform needed.
    uvec2 pixel=uvec2(gl_FragCoord.xy);
    uint seed=displayHash(pixel.x^displayHash(pixel.y+0x9e3779b9u));
    float dither=((float(seed>>8u)+.5)*(1.0/16777216.0)-.5)/255.0;
    outColor=vec4(clamp(encoded+vec3(dither),vec3(0),vec3(1)),1);
}
