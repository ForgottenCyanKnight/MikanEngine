#version 450
// Diagnostic-only G-buffer color view; scene/material data is not altered.
layout(location=0) in vec2 uv;
layout(binding=0) uniform sampler2D albedoImage;
layout(location=0) out vec4 color;
void main(){color=vec4(texture(albedoImage,uv).rgb,1);}
