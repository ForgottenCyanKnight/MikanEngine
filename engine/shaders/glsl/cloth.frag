#version 450
#extension GL_GOOGLE_include_directive : require
#include "triangle_debug_begin.glsl"

layout(location = 0) in vec4 vertexColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vertexColor;
}

#include "triangle_debug_end.glsl"
