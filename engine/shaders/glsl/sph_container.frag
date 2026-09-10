#version 450

layout(location = 0) out vec4 fragColor;

void main() {
    // The walls are intentionally subtle: they define a closed volume while
    // pressure-colored particles remain visible through the container.
    fragColor = vec4(0.08, 0.30, 0.82, 0.025);
}
