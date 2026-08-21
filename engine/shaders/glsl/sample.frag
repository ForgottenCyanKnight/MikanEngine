#version 450
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // composite£¨È«³ß´çÏßÐÔ HDR£©

void main() {
    vec3 color = texture(inputTex, fragTexCoord).rgb;
    fragColor = vec4(color, 1.0);
}
