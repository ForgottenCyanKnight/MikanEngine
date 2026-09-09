#version 450
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    vec3 c = vec3(0.0);
    for (int j = -8; j <= 8; j++)
        c += texture(inputTex, fragTexCoord + vec2(0.0, texel.y * float(j))).rgb;
    fragColor = vec4(c / 17.0, 1.0);
}
