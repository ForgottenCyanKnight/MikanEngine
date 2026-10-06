#version 450
#extension GL_GOOGLE_include_directive : require
#include "vox_rt_color.glsl"
layout(location=0) in vec2 fragUV;
layout(binding=0) uniform sampler2D hardwareImage;
layout(location=0) out vec4 outColor;
void main(){
    vec3 hdr=max(texture(hardwareImage,fragUV).rgb,vec3(0));
    // Current editor display and requested swapchain attachments are UNORM:
    // encode exactly once here. A future SRGB attachment must skip this encode.
    outColor=vec4(rtLinearToSrgb(rtDisplayMap(hdr)),1);
}
