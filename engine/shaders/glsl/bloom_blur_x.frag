#version 450
// weights = 0.27343750, 0.21875000, 0.10937500, 0.03125000, 0.00390625（HSPE 同款）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

void main() {
    ivec2 iuv = ivec2(gl_FragCoord.xy);
    vec2 texSize = vec2(textureSize(inputTex, 0));
    vec3 sum = texture(inputTex, (vec2(iuv) + 0.5) / texSize).rgb * 0.27343750;
    sum += (texture(inputTex, (vec2(iuv + ivec2(1, 0)) + 0.5) / texSize).rgb + texture(inputTex, (vec2(iuv - ivec2(1, 0)) + 0.5) / texSize).rgb) * 0.21875000;
    sum += (texture(inputTex, (vec2(iuv + ivec2(2, 0)) + 0.5) / texSize).rgb + texture(inputTex, (vec2(iuv - ivec2(2, 0)) + 0.5) / texSize).rgb) * 0.10937500;
    sum += (texture(inputTex, (vec2(iuv + ivec2(3, 0)) + 0.5) / texSize).rgb + texture(inputTex, (vec2(iuv - ivec2(3, 0)) + 0.5) / texSize).rgb) * 0.03125000;
    sum += (texture(inputTex, (vec2(iuv + ivec2(4, 0)) + 0.5) / texSize).rgb + texture(inputTex, (vec2(iuv - ivec2(4, 0)) + 0.5) / texSize).rgb) * 0.00390625;
    fragColor = vec4(sum, 1.0);
}
